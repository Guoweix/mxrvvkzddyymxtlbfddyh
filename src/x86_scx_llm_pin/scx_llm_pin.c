// SPDX-License-Identifier: GPL-2.0
/*
 * scx_llm_pin - User-space loader
 *
 * 用法:
 *   scx_llm_pin --tgid <pid> [--duration 60]
 */
#define _GNU_SOURCE
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <getopt.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <time.h>
#include <limits.h>
#include <linux/bpf.h>
#include <bpf/libbpf.h>
#include <bpf/bpf.h>
#include "scx_llm_pin.h"
#include "team_config.h"
#ifdef SCX_LLM_SKEL_HEADER
#include SCX_LLM_SKEL_HEADER
#else
#include "scx_llm_pin.skel.h"
#endif

#define NSEC_PER_MSEC 1000000ULL
#ifndef BPF_F_SLEEPABLE
#define BPF_F_SLEEPABLE (1U << 4)
#endif

static volatile sig_atomic_t exiting = 0;

static void on_signal(int sig)
{
	(void)sig;
	exiting = 1;
}

static int libbpf_log(enum libbpf_print_level level, const char *fmt, va_list args)
{
	if (level == LIBBPF_DEBUG)
		return 0;
	return vfprintf(stderr, fmt, args);
}

static const char *phase_name(__u32 phase)
{
	switch (phase) {
	case RVV_LEASE_PHASE_IDLE:
		return "idle";
	case RVV_LEASE_PHASE_PREFILL:
		return "prefill";
	case RVV_LEASE_PHASE_DECODE:
		return "decode";
	case RVV_LEASE_PHASE_MIXED:
		return "mixed";
	default:
		return "invalid";
	}
}

static int parse_on_off(const char *str, __u32 *enabled)
{
	if (!strcmp(str, "on"))
		*enabled = 1;
	else if (!strcmp(str, "off"))
		*enabled = 0;
	else
		return -EINVAL;
	return 0;
}

static const char *x86_smt_mode_name(__u32 mode)
{
	switch (mode) {
	case LLM_X86_SMT_OFF:
		return "off";
	case LLM_X86_SMT_ADAPTIVE:
		return "adaptive";
	default:
		return "unknown";
	}
}

static int parse_x86_smt_mode(const char *str, __u32 *mode)
{
	if (!strcmp(str, "off"))
		*mode = LLM_X86_SMT_OFF;
	else if (!strcmp(str, "adaptive"))
		*mode = LLM_X86_SMT_ADAPTIVE;
	else
		return -EINVAL;
	return 0;
}

#ifdef SCX_ARCH_X86
static int read_cpu_topology_value(unsigned cpu, const char *name, int *value)
{
	char path[PATH_MAX];
	FILE *file;
	int err;

	err = snprintf(path, sizeof(path),
		       "/sys/devices/system/cpu/cpu%u/topology/%s", cpu, name);
	if (err < 0 || (size_t)err >= sizeof(path))
		return -ENAMETOOLONG;
	file = fopen(path, "r");
	if (!file)
		return -errno;
	err = fscanf(file, "%d", value) == 1 ? 0 : -EINVAL;
	fclose(file);
	return err;
}

static int read_cpu_topology_string(unsigned cpu, const char *name,
				    char *buf, size_t size)
{
	char path[PATH_MAX];
	FILE *file;
	int err;

	err = snprintf(path, sizeof(path),
		       "/sys/devices/system/cpu/cpu%u/topology/%s", cpu, name);
	if (err < 0 || (size_t)err >= sizeof(path))
		return -ENAMETOOLONG;
	file = fopen(path, "r");
	if (!file)
		return -errno;
	if (!fgets(buf, (int)size, file)) {
		fclose(file);
		return -EINVAL;
	}
	fclose(file);
	return 0;
}

/*
 * Validate the i5-10200H 4C/8T layout: CPUs 0..3 are the primary lanes,
 * CPUs 4..7 their SMT siblings, with sibling pairs 0/4, 1/5, 2/6, 3/7.
 */
static int validate_x86_4c8t_topology(void)
{
	struct {
		int package;
		int core;
	} cpu[8];

	for (unsigned i = 0; i < 8; i++) {
		if (read_cpu_topology_value(i, "physical_package_id", &cpu[i].package) ||
		    read_cpu_topology_value(i, "core_id", &cpu[i].core))
			return -EINVAL;
	}
	for (unsigned i = 0; i < 4; i++) {
		char primary[64], sibling[64], expect[64];

		if (cpu[i].package != cpu[i + 4].package ||
		    cpu[i].core != cpu[i + 4].core)
			return -EINVAL;
		for (unsigned j = i + 1; j < 4; j++)
			if (cpu[i].package == cpu[j].package &&
			    cpu[i].core == cpu[j].core)
				return -EINVAL;
		/* thread_siblings_list must read "i,i+4" for both lanes. */
		if (read_cpu_topology_string(i, "thread_siblings_list",
					     primary, sizeof(primary)) ||
		    read_cpu_topology_string(i + 4, "thread_siblings_list",
					     sibling, sizeof(sibling)))
			return -EINVAL;
		snprintf(expect, sizeof(expect), "%u,%u", i, i + 4);
		primary[strcspn(primary, "\r\n")] = '\0';
		sibling[strcspn(sibling, "\r\n")] = '\0';
		if (strcmp(primary, sibling) || strcmp(primary, expect))
			return -EINVAL;
	}
	return 0;
}
#endif

static __u64 monotonic_ns(void)
{
	struct timespec ts = {};

	if (clock_gettime(CLOCK_MONOTONIC, &ts))
		return 0;
	return (__u64)ts.tv_sec * 1000000000ULL + (__u64)ts.tv_nsec;
}

static __u64 new_control_session_id(void)
{
	struct timespec ts = {};
	__u64 value;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	value = ((__u64)ts.tv_sec << 32) ^ (__u64)ts.tv_nsec ^
		((__u64)getpid() << 16);
	return value ? value : 1;
}

static __u32 config_team_tgid(const struct llm_pin_config *config, __u32 team)
{
	return team < config->team_count ? config->teams[team].tgid : 0;
}

static int initialize_and_pin_control_maps(
	struct scx_llm_pin_bpf *skel, const char *dir,
	const struct llm_pin_config *config, char *config_path,
	size_t config_path_size, char *worker_path, size_t worker_path_size)
{
	struct rvv_lease_worker_page page = {};
	__u32 key;
	int err;

	if (mkdir(dir, 0700) && errno != EEXIST)
		return -errno;
	if (snprintf(config_path, config_path_size, "%s/%s", dir,
		     RVV_LEASE_CONFIG_MAP_NAME) >= (int)config_path_size ||
	    snprintf(worker_path, worker_path_size, "%s/%s", dir,
		     RVV_LEASE_WORKER_MAP_NAME) >= (int)worker_path_size)
		return -ENAMETOOLONG;
	if (!access(config_path, F_OK) || !access(worker_path, F_OK)) {
		fprintf(stderr, "control map path already exists under %s\n", dir);
		return -EEXIST;
	}

	for (key = 0; key < RVV_LEASE_CONTROL_TEAMS; key++) {
		__u32 target_tgid = config_team_tgid(config, key);

		memset(&page, 0, sizeof(page));
		if (target_tgid) {
			page.header.magic = RVV_LEASE_CONTROL_MAGIC;
			page.header.abi_version = RVV_LEASE_CONTROL_ABI_VERSION;
			page.header.active = 1;
			page.header.target_tgid = target_tgid;
			page.header.max_workers = RVV_LEASE_CONTROL_WORKERS;
			page.header.worker_limit = config->teams[key].threads > config->teams[key].threads_batch ?
				config->teams[key].threads : config->teams[key].threads_batch;
			page.header.loader_pid = (__u32)getpid();
			page.header.team_id = key;
			page.header.hint_flags = RVV_LEASE_MODEL_ACTIVE;
			page.header.session_id = config->user_session_id;
			page.header.policy_epoch = config->user_policy_epoch;
			page.team_hint.phase = RVV_LEASE_PHASE_IDLE;
			page.team_hint.session_id = config->user_session_id;
		}
		err = bpf_map__update_elem(skel->maps.user_worker_map, &key,
					   sizeof(key), &page, sizeof(page), BPF_ANY);
		if (err)
			return err;
	}
	err = bpf_map__pin(skel->maps.config_map, config_path);
	if (err)
		return err;
	err = bpf_map__pin(skel->maps.user_worker_map, worker_path);
	if (err) {
		bpf_map__unpin(skel->maps.config_map, config_path);
		return err;
	}
	chmod(config_path, 0600);
	chmod(worker_path, 0600);
	return 0;
}

static void deactivate_and_unpin_control_maps(
	struct scx_llm_pin_bpf *skel, const char *dir, const char *config_path,
	const char *worker_path, bool pinned)
{
	struct rvv_lease_worker_page page = {};
	__u32 key;

	if (!pinned || !skel)
		return;
	for (key = 0; key < RVV_LEASE_CONTROL_TEAMS; key++) {
		if (!bpf_map__lookup_elem(skel->maps.user_worker_map, &key,
					  sizeof(key), &page, sizeof(page), 0)) {
			page.header.active = 0;
			bpf_map__update_elem(skel->maps.user_worker_map, &key,
					     sizeof(key), &page, sizeof(page), BPF_ANY);
		}
	}
	bpf_map__unpin(skel->maps.user_worker_map, worker_path);
	bpf_map__unpin(skel->maps.config_map, config_path);
	rmdir(dir);
}

static void print_worker_page(struct scx_llm_pin_bpf *skel)
{
	struct rvv_lease_worker_page page = {};
	__u32 key;

	for (key = 0; key < RVV_LEASE_CONTROL_TEAMS; key++) {
		if (bpf_map__lookup_elem(skel->maps.user_worker_map, &key,
					 sizeof(key), &page, sizeof(page), 0) ||
		    !page.header.active)
			continue;
		printf("\n=== userspace lease workers team=%u ===\n", key);
		printf("active=%u tgid=%u session=%llu epoch=%llu abi=%u\n",
		       page.header.active, page.header.target_tgid,
		       (unsigned long long)page.header.session_id,
		       (unsigned long long)page.header.policy_epoch,
		       page.header.abi_version);
		printf("team_hint team=%u phase=%s membership_epoch=%llu phase_epoch=%llu workers=%u prompt=%u decode=%u begin_ns=%llu expires_ns=%llu seq=%u\n",
		       key, phase_name(page.team_hint.phase),
		       (unsigned long long)page.team_hint.membership_epoch,
		       (unsigned long long)page.team_hint.phase_epoch,
		       page.team_hint.active_workers, page.team_hint.prompt_tokens,
		       page.team_hint.decode_tokens,
		       (unsigned long long)page.team_hint.begin_ns,
		       (unsigned long long)page.team_hint.expires_ns,
		       page.team_hint.sequence);
		for (__u32 ith = 0; ith < RVV_LEASE_CONTROL_WORKERS; ith++) {
			const struct rvv_lease_worker_slot *slot = &page.workers[ith];

			if (!slot->sequence || !slot->tid)
				continue;
			printf("lease_worker team=%u ith=%u tid=%u tgid=%u n_threads=%u membership_epoch=%llu session=%llu seq=%u\n",
			       key, slot->ith, slot->tid, slot->tgid, slot->n_threads,
			       (unsigned long long)slot->membership_epoch,
			       (unsigned long long)slot->session_id, slot->sequence);
		}
	}
	fflush(stdout);
}

static void print_stats(struct scx_llm_pin_bpf *skel)
{
	struct llm_pin_stats stats = {0};
	__u32 key = 0;
	int err;

	err = bpf_map__lookup_elem(skel->maps.stats_map, &key, sizeof(key),
				    &stats, sizeof(stats), 0);
	if (err) {
		fprintf(stderr, "failed to read stats: %d\n", err);
		return;
	}

	printf("\n=== scx_llm_pin statistics ===\n");
	printf("LLM tasks scheduled:    %llu\n", stats.llm_scheduled);
	printf("LLM tasks pinned:       %llu\n", stats.llm_pinned);
	printf("System tasks scheduled: %llu\n", stats.system_scheduled);
	printf("CPU migrations:         %llu\n", stats.cpu_migrations);
	printf("LLM local dispatches:   %llu\n", stats.llm_local_dispatches);
	printf("System dispatches:      %llu\n", stats.system_dispatches);
	printf("RVV dirty samples:      %llu\n", stats.rvv_dirty_samples);
	printf("RVV vector threads:     %llu\n", stats.rvv_vector_threads);
	printf("RVV vector threads by team: %llu/%llu\n",
	       stats.rvv_vector_threads_team0, stats.rvv_vector_threads_team1);
	printf("RVV vector dispatches:  %llu\n", stats.rvv_vector_dispatches);
	printf("RVV nonvector dispatches:  %llu\n", stats.rvv_nonvector_llm_dispatches);
	printf("RVV vector DSQ dispatches:   %llu\n", stats.rvv_vector_dsq_dispatches);
	printf("RVV vector DSQ consumes:     %llu\n", stats.rvv_vector_dsq_consumes);
	printf("RVV vector DSQ dispatches by team: %llu/%llu\n",
	       stats.rvv_vector_dsq_dispatches_team0,
	       stats.rvv_vector_dsq_dispatches_team1);
	printf("RVV vector DSQ consumes by team:   %llu/%llu\n",
	       stats.rvv_vector_dsq_consumes_team0,
	       stats.rvv_vector_dsq_consumes_team1);
	printf("RVV vector slice clamps:     %llu\n", stats.rvv_vector_slice_clamps);
	printf("RVV vector local dispatches: %llu\n", stats.rvv_vector_local_dispatches);
	printf("RVV vector window starts:    %llu\n", stats.rvv_vector_window_starts);
	printf("Phase hint reads valid/invalid: %llu/%llu/%llu\n",
	       stats.phase_hint_reads, stats.phase_hint_valid,
	       stats.phase_hint_invalid);
	printf("Phase hint idle/prefill/decode/mixed: %llu/%llu/%llu/%llu\n",
	       stats.phase_hint_idle, stats.phase_hint_prefill,
	       stats.phase_hint_decode, stats.phase_hint_mixed);
	printf("Phase lease engagements/preferred/empty/fallback: %llu/%llu/%llu/%llu\n",
	       stats.phase_lease_engagements,
	       stats.phase_lease_preferred_consumes,
	       stats.phase_lease_empty_fallbacks,
	       stats.phase_lease_fallback_consumes);
	printf("Phase lease safety forced/miss/stale: %llu/%llu/%llu\n",
	       stats.phase_lease_safety_forced,
	       stats.phase_lease_safety_misses,
	       stats.phase_lease_pending_stale);
	printf("Phase lease pending max/ge4s: %lluns/%llu\n",
	       stats.phase_lease_pending_max_ns,
	       stats.phase_lease_pending_ge_4s);
	printf("Phase lease runs total/preferred/other: %llu/%llu/%llu\n",
	       stats.phase_lease_running_samples,
	       stats.phase_lease_preferred_runs,
	       stats.phase_lease_other_runs);
	printf("Phase lease wakeup candidates/preempts: %llu/%llu\n",
	       stats.phase_lease_wakeup_candidates,
	       stats.phase_lease_wakeup_preempts);
	printf("x86 SMT primary/sibling selections: %llu/%llu\n",
	       stats.x86_smt_primary_selections,
	       stats.x86_smt_sibling_selections);
	printf("x86 SMT latency-primary/decode-shared: %llu/%llu\n",
	       stats.x86_smt_latency_primary,
	       stats.x86_smt_decode_shared);
	printf("SCX_PHASE_STATS phase_reads=%llu phase_valid=%llu phase_invalid=%llu phase_idle=%llu phase_prefill=%llu phase_decode=%llu phase_mixed=%llu engagements=%llu preferred=%llu empty=%llu fallback=%llu safety_forced=%llu safety_misses=%llu pending_stale=%llu pending_max_ns=%llu pending_ge_4s=%llu runs=%llu preferred_runs=%llu other_runs=%llu wakeup_candidates=%llu wakeup_preempts=%llu\n",
	       stats.phase_hint_reads, stats.phase_hint_valid,
	       stats.phase_hint_invalid, stats.phase_hint_idle,
	       stats.phase_hint_prefill, stats.phase_hint_decode,
	       stats.phase_hint_mixed, stats.phase_lease_engagements,
	       stats.phase_lease_preferred_consumes,
	       stats.phase_lease_empty_fallbacks,
	       stats.phase_lease_fallback_consumes,
	       stats.phase_lease_safety_forced,
	       stats.phase_lease_safety_misses,
	       stats.phase_lease_pending_stale,
	       stats.phase_lease_pending_max_ns,
	       stats.phase_lease_pending_ge_4s,
	       stats.phase_lease_running_samples,
	       stats.phase_lease_preferred_runs,
	       stats.phase_lease_other_runs,
	       stats.phase_lease_wakeup_candidates,
	       stats.phase_lease_wakeup_preempts);
	printf("SCX_RVV_STATS rvv_team0=%llu rvv_team1=%llu vector_dispatches=%llu vector_dsq_dispatches=%llu vector_dsq_consumes=%llu dsq_dispatches_team0=%llu dsq_dispatches_team1=%llu dsq_consumes_team0=%llu dsq_consumes_team1=%llu slice_clamps=%llu local_dispatches=%llu window_starts=%llu\n",
	       stats.rvv_vector_threads_team0, stats.rvv_vector_threads_team1,
	       stats.rvv_vector_dispatches,
	       stats.rvv_vector_dsq_dispatches,
	       stats.rvv_vector_dsq_consumes,
	       stats.rvv_vector_dsq_dispatches_team0,
	       stats.rvv_vector_dsq_dispatches_team1,
	       stats.rvv_vector_dsq_consumes_team0,
	       stats.rvv_vector_dsq_consumes_team1,
	       stats.rvv_vector_slice_clamps,
	       stats.rvv_vector_local_dispatches,
	       stats.rvv_vector_window_starts);
	for (unsigned t = 0; t < RVV_LEASE_CONTROL_TEAMS; t++) {
		printf("SCX_TEAM_STATS team=%u workers=%llu enqueues=%llu consumes=%llu runs=%llu\n",
		       t, (unsigned long long)stats.teams[t].vector_threads,
		       (unsigned long long)stats.teams[t].enqueues,
		       (unsigned long long)stats.teams[t].consumes,
		       (unsigned long long)stats.teams[t].runs);
	}
	fflush(stdout);
}

static void run_main_loop(struct scx_llm_pin_bpf *skel, int duration)
{
	__u64 start_ns = monotonic_ns();
	__u64 report_ns = start_ns + 10ULL * 1000000000ULL;
	__u64 deadline_ns = duration > 0 ?
		start_ns + (__u64)duration * 1000000000ULL : 0;

	while (!exiting && (!deadline_ns || monotonic_ns() < deadline_ns)) {
		sleep(duration > 0 ? 1 : 5);
		if (monotonic_ns() >= report_ns) {
			print_stats(skel);
			print_worker_page(skel);
			report_ns = monotonic_ns() + 10ULL * 1000000000ULL;
		}
	}
}

enum {
	OPT_TEAM = 1000,
	OPT_CHECK_CONFIG,
	OPT_TGID2,
	OPT_PHASE_HINT_TIMEOUT_MS,
	OPT_PREFILL_STARVATION_MS,
	OPT_PHASE_WAKEUP_PREEMPT,
	OPT_X86_SMT,
};

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage: %s [options]\n"
		"\n"
		"Options:\n"
		"  --team PID:T:TB[:MASK] Repeat for 1..8 models; T/TB <= allowed CPUs (max 8)\n"
		"  --check-config        Validate and print configuration without loading BPF\n"
		"  --tgid <pid>           Target process TGID (llama-server PID)\n"
		"  --tgid2 <pid>          Optional second llama-server TGID\n"
		"  --vector-slice-ms <ms> RVV vector worker slice (default: 100)\n"
		"  --vector-local-window-ms <ms> RVV local fast-path window after vector DSQ consume (default: 500)\n"
		"  --dirty-threshold <n>  Dirty samples before marking vector thread (default: 2)\n"
		"  --phase-hint-timeout-ms <ms> Reject stale phase hints (default: 300000)\n"
		"  --prefill-starvation-ms <ms> V DSQ safety wait for prefill/MIXED lease (default: 1000)\n"
		"  --phase-wakeup-preempt <on|off> Preferred wakeup targeted preemption (default: on)\n"
		"  --x86-smt <mode>       off|adaptive (x86 i5-10200H 4C/8T; default: off)\n"
		"  --duration <sec>       Run duration in seconds (0 = infinite, default: 0)\n"
		"  -h, --help             Show this help\n"
		"\n"
		"Example:\n"
		"  %s --tgid 1234 --vector-slice-ms 100\n"
		"\n",
		prog, prog);
}

int main(int argc, char **argv)
{
	struct scx_llm_pin_bpf *skel = NULL;
	struct bpf_link *link = NULL;
	struct llm_pin_config config = {0};
	const char *control_dir = RVV_LEASE_CONTROL_DEFAULT_DIR;
	char config_map_path[PATH_MAX] = {};
	char worker_map_path[PATH_MAX] = {};
	int duration = 0;
	bool control_maps_pinned = false;
	bool check_config = false;
	int err;
	__u32 key = 0;

	config.dirty_threshold = 2;
	config.vector_local_window_ns = 500 * NSEC_PER_MSEC;
	config.phase_wakeup_preempt_enabled = 1;
	config.phase_hint_timeout_ns = 300000 * NSEC_PER_MSEC;
	config.prefill_starvation_ns = 1000 * NSEC_PER_MSEC;
	config.x86_smt_mode = LLM_X86_SMT_OFF;

	static struct option long_options[] = {
		{"team", required_argument, 0, OPT_TEAM},
		{"check-config", no_argument, 0, OPT_CHECK_CONFIG},
		{"tgid",        required_argument, 0, 't'},
		{"tgid2",       required_argument, 0, OPT_TGID2},
		{"vector-slice-ms", required_argument, 0, 'M'},
		{"vector-local-window-ms", required_argument, 0, 'W'},
		{"dirty-threshold", required_argument, 0, 'D'},
		{"phase-hint-timeout-ms", required_argument, 0, OPT_PHASE_HINT_TIMEOUT_MS},
		{"prefill-starvation-ms", required_argument, 0, OPT_PREFILL_STARVATION_MS},
		{"phase-wakeup-preempt", required_argument, 0, OPT_PHASE_WAKEUP_PREEMPT},
		{"x86-smt", required_argument, 0, OPT_X86_SMT},
		{"duration",    required_argument, 0, 'd'},
		{"help",        no_argument,       0, 'h'},
		{0, 0, 0, 0}
	};

	// 解析命令行参数
	while (1) {
		int c = getopt_long(argc, argv, "h", long_options, NULL);
		if (c == -1)
			break;

		switch (c) {
		case OPT_TEAM:
			if (config.team_count >= RVV_LEASE_CONTROL_TEAMS ||
			    llm_parse_team(optarg, &config.teams[config.team_count])) {
				fprintf(stderr, "Error: --team requires PID:T:TB[:MASK], T/TB in 1..allowed cores (max 8); at most 8 models\n");
				return 1;
			}
			config.team_count++;
			break;
		case OPT_CHECK_CONFIG:
			check_config = true;
			break;
		case 't':
			config.target_tgid = atoi(optarg);
			break;
		case OPT_TGID2:
			config.target_tgid2 = atoi(optarg);
			break;
		case 'M':
			config.vector_slice_ns = strtoull(optarg, NULL, 10) * NSEC_PER_MSEC;
			break;
		case 'W':
			config.vector_local_window_ns = strtoull(optarg, NULL, 10) * NSEC_PER_MSEC;
			break;
		case 'D':
			config.dirty_threshold = strtoul(optarg, NULL, 10);
			break;
		case OPT_PHASE_HINT_TIMEOUT_MS:
			config.phase_hint_timeout_ns = strtoull(optarg, NULL, 10) * NSEC_PER_MSEC;
			break;
		case OPT_PREFILL_STARVATION_MS:
			config.prefill_starvation_ns = strtoull(optarg, NULL, 10) * NSEC_PER_MSEC;
			break;
		case OPT_PHASE_WAKEUP_PREEMPT:
			if (parse_on_off(optarg,
					 &config.phase_wakeup_preempt_enabled)) {
				fprintf(stderr, "Error: --phase-wakeup-preempt requires on|off\n");
				return 1;
			}
			break;
		case OPT_X86_SMT:
			if (parse_x86_smt_mode(optarg, &config.x86_smt_mode)) {
				fprintf(stderr, "Error: --x86-smt requires off|adaptive\n");
				return 1;
			}
			break;
		case 'd':
			duration = atoi(optarg);
			break;
		case 'h':
		default:
			usage(argv[0]);
			return c == 'h' ? 0 : 1;
		}
	}

	// 参数验证
	if (config.team_count && (config.target_tgid || config.target_tgid2)) {
		fprintf(stderr, "Error: use --team or legacy --tgid/--tgid2, not both\n");
		return 1;
	}
	if (!config.team_count && config.target_tgid) {
		config.team_count = config.target_tgid2 ? 2 : 1;
		for (unsigned t = 0; t < config.team_count; t++) {
			config.teams[t].tgid = t ? config.target_tgid2 : config.target_tgid;
			config.teams[t].threads = config.teams[t].threads_batch = 8;
			config.teams[t].cpu_mask = LLM_CPU_MASK;
		}
	}
	if (llm_prepare_teams(&config)) {
		fprintf(stderr, "Error: invalid/duplicate model configuration; supply --team or --tgid\n");
		return 1;
	}
	if (!config.target_tgid) {
		fprintf(stderr, "Error: --tgid is required\n\n");
		usage(argv[0]);
		return 1;
	}
	if (config.target_tgid2 == config.target_tgid) {
		fprintf(stderr, "Error: --tgid2 must differ from --tgid\n");
		return 1;
	}
	if (!config.dirty_threshold)
		config.dirty_threshold = 2;
	if (!config.vector_slice_ns)
		config.vector_slice_ns = 100 * NSEC_PER_MSEC;
	if (config.prefill_starvation_ns < 100 * NSEC_PER_MSEC ||
	    config.prefill_starvation_ns > 4000 * NSEC_PER_MSEC) {
		fprintf(stderr, "Error: --prefill-starvation-ms must be in 100..4000\n");
		return 1;
	}
	if (config.x86_smt_mode > LLM_X86_SMT_ADAPTIVE) {
		fprintf(stderr, "Error: invalid x86 SMT mode\n");
		return 1;
	}
	if (config.x86_smt_mode == LLM_X86_SMT_ADAPTIVE) {
#ifndef SCX_ARCH_X86
		fprintf(stderr, "Error: --x86-smt adaptive is available only in the x86 build\n");
		return 1;
#else
		/*
		 * The current single-strategy design is always hint-classified and
		 * always phase-aware, so adaptive only needs the 4C/8T shape.
		 */
		if (config.team_count != 2) {
			fprintf(stderr, "Error: --x86-smt adaptive requires exactly two teams\n");
			return 1;
		}
		for (unsigned t = 0; t < config.team_count; t++) {
			unsigned n = config.teams[t].threads > config.teams[t].threads_batch ?
				config.teams[t].threads : config.teams[t].threads_batch;

			if (n > 4 || config.teams[t].cpu_mask != 0xff) {
				fprintf(stderr, "Error: --x86-smt adaptive requires T/TB <= 4 and CPU mask 0xff for every team\n");
				return 1;
			}
		}
		if (validate_x86_4c8t_topology()) {
			fprintf(stderr, "Error: --x86-smt adaptive requires sibling pairs 0/4, 1/5, 2/6 and 3/7\n");
			return 1;
		}
#endif
	}

	config.enabled = 1;
	config.user_session_id = new_control_session_id();
	config.user_policy_epoch = 1;

	// 打印配置
	printf("scx_llm_pin configuration:\n");
	printf("  Target TGID:     %u\n", config.target_tgid);
	if (config.target_tgid2)
		printf("  Target TGID 2:   %u\n", config.target_tgid2);
	printf("  Vector slice:    %llums\n",
	       (unsigned long long)(config.vector_slice_ns / NSEC_PER_MSEC));
	printf("  Vector local window: %llums\n",
	       (unsigned long long)(config.vector_local_window_ns / NSEC_PER_MSEC));
	printf("  Dirty threshold: %u\n", config.dirty_threshold);
	printf("  Phase timeout:   %llums\n",
	       (unsigned long long)(config.phase_hint_timeout_ns / NSEC_PER_MSEC));
	printf("  Prefill starvation guard: %llums\n",
	       (unsigned long long)(config.prefill_starvation_ns / NSEC_PER_MSEC));
	printf("  Phase wakeup preempt: %s\n",
	       config.phase_wakeup_preempt_enabled ? "on" : "off");
	printf("  x86 SMT policy:  %s\n", x86_smt_mode_name(config.x86_smt_mode));
	printf("  Control dir:     %s\n", control_dir);
	printf("  Control session: %llu\n",
	       (unsigned long long)config.user_session_id);
	printf("  Duration:        %s\n", duration ? "limited" : "infinite");
	printf("\n");

	for (unsigned t = 0; t < config.team_count; t++) {
		printf("TEAM_CONFIG team=%u tgid=%u threads=%u threads_batch=%u cpu_mask=0x%x cpu_order=",
		       t, config.teams[t].tgid, config.teams[t].threads,
		       config.teams[t].threads_batch, config.teams[t].cpu_mask);
		for (unsigned i = 0;
		     i < (unsigned)__builtin_popcount(config.teams[t].cpu_mask); i++)
			printf("%s%u", i ? "," : "", config.teams[t].cpu_order[i]);
		printf("\n");
	}
	if (check_config)
		return 0;
	for (unsigned t = 0; t < config.team_count; t++) {
		cpu_set_t allowed;

		CPU_ZERO(&allowed);
		if (sched_getaffinity(config.teams[t].tgid, sizeof(allowed), &allowed)) {
			perror("target sched_getaffinity");
			return 1;
		}
		for (unsigned cpu = 0; cpu < RVV_LEASE_MAX_CPUS; cpu++) {
			if ((config.teams[t].cpu_mask & (1U << cpu)) &&
			    !CPU_ISSET(cpu, &allowed)) {
				fprintf(stderr, "Error: team CPU mask must fit target process affinity/online CPUs\n");
				return 1;
			}
		}
	}

	// 信号处理
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	libbpf_set_print(libbpf_log);

	// 加载 BPF 程序。init 里创建自定义 DSQ，需要 sleepable flag。
	skel = scx_llm_pin_bpf__open();
	if (!skel) {
		fprintf(stderr, "failed to open scx_llm_pin BPF object\n");
		return 1;
	}
	err = bpf_program__set_flags(skel->progs.scx_llm_pin_init, BPF_F_SLEEPABLE);
	if (err) {
		fprintf(stderr, "failed to mark init program sleepable: %d\n", err);
		goto cleanup;
	}
	err = scx_llm_pin_bpf__load(skel);
	if (err) {
		fprintf(stderr, "failed to load scx_llm_pin BPF object: %d\n", err);
		goto cleanup;
	}

	// 写入配置
	err = bpf_map__update_elem(skel->maps.config_map, &key, sizeof(key),
				    &config, sizeof(config), BPF_ANY);
	if (err) {
		fprintf(stderr, "failed to write config: %d\n", err);
		goto cleanup;
	}
	err = initialize_and_pin_control_maps(
		skel, control_dir, &config, config_map_path,
		sizeof(config_map_path), worker_map_path,
		sizeof(worker_map_path));
	if (err) {
		fprintf(stderr, "failed to initialize userspace control maps: %d\n", err);
		goto cleanup;
	}
	control_maps_pinned = true;
	printf("RVV lease control maps ready: config=%s workers=%s session=%llu\n",
	       config_map_path, worker_map_path,
	       (unsigned long long)config.user_session_id);

	// 附加 struct_ops
	link = bpf_map__attach_struct_ops(skel->maps.scx_llm_pin_ops);
	if (!link) {
		err = -errno;
		fprintf(stderr, "failed to attach sched_ext struct_ops: %d\n", err);
		goto cleanup;
	}

	printf("scx_llm_pin attached successfully\n");
	printf("Press Ctrl-C to detach\n\n");
	fflush(stdout);

	run_main_loop(skel, duration);

	// 打印最终统计
	print_stats(skel);
	print_worker_page(skel);

cleanup:
	bpf_link__destroy(link);
	deactivate_and_unpin_control_maps(skel, control_dir, config_map_path,
					  worker_map_path, control_maps_pinned);
	scx_llm_pin_bpf__destroy(skel);
	printf("\nscx_llm_pin detached\n");
	return err < 0 ? -err : 0;
}
