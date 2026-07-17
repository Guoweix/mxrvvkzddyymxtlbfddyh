// SPDX-License-Identifier: GPL-2.0
/*
 * scx_llm_pin - User-space loader
 *
 * 用法:
 *   scx_llm_pin --tgid <pid> --llm-cpus 0-3 --system-cpus 4-7 [--duration 60]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <getopt.h>
#include <stdarg.h>
#include <dirent.h>
#include <sched.h>
#include <sys/stat.h>
#include <time.h>
#include <limits.h>
#include <linux/bpf.h>
#include <bpf/libbpf.h>
#include <bpf/bpf.h>
#include "scx_llm_pin.h"
#include "scx_llm_pin.skel.h"

#define NSEC_PER_MSEC 1000000ULL
#ifndef BPF_F_SLEEPABLE
#define BPF_F_SLEEPABLE (1U << 4)
#endif
#ifndef SCHED_EXT
#define SCHED_EXT 7
#endif
#ifndef SCHED_NORMAL
#define SCHED_NORMAL 0
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

static const char *control_mode_name(__u32 mode)
{
	switch (mode) {
	case RVV_LEASE_CONTROL_OFF:
		return "off";
	case RVV_LEASE_CONTROL_OBSERVE:
		return "observe";
	case RVV_LEASE_CONTROL_ACTIVE:
		return "active";
	default:
		return "unknown";
	}
}

static int parse_control_mode(const char *str, __u32 *mode)
{
	if (!strcmp(str, "off"))
		*mode = RVV_LEASE_CONTROL_OFF;
	else if (!strcmp(str, "observe"))
		*mode = RVV_LEASE_CONTROL_OBSERVE;
	else if (!strcmp(str, "active"))
		*mode = RVV_LEASE_CONTROL_ACTIVE;
	else
		return -EINVAL;
	return 0;
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

static int parse_cpu_order(const char *str,
			   __u32 cpus[RVV_LEASE_CONTROL_WORKERS],
			   __u32 *enabled)
{
	bool seen[RVV_LEASE_CONTROL_WORKERS] = {};
	char *saveptr = NULL;
	char *token;
	char *tmp;
	__u32 count = 0;

	if (!strcmp(str, "auto")) {
		*enabled = 0;
		return 0;
	}

	tmp = strdup(str);
	if (!tmp)
		return -ENOMEM;
	for (token = strtok_r(tmp, ",", &saveptr); token;
	     token = strtok_r(NULL, ",", &saveptr)) {
		char *end = NULL;
		long cpu;

		if (count >= RVV_LEASE_CONTROL_WORKERS) {
			free(tmp);
			return -EINVAL;
		}
		errno = 0;
		cpu = strtol(token, &end, 10);
		if (errno || !end || *end != '\0' || cpu < 0 ||
		    cpu >= RVV_LEASE_CONTROL_WORKERS || seen[cpu]) {
			free(tmp);
			return -EINVAL;
		}
		seen[cpu] = true;
		cpus[count++] = (__u32)cpu;
	}
	free(tmp);
	if (count != RVV_LEASE_CONTROL_WORKERS)
		return -EINVAL;
	*enabled = 1;
	return 0;
}

static int initialize_and_pin_control_maps(
	struct scx_llm_pin_bpf *skel, const char *dir,
	const struct llm_pin_config *config, char *config_path,
	size_t config_path_size, char *worker_path, size_t worker_path_size)
{
	struct rvv_lease_worker_page page = {};
	__u32 key = 0;
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

	page.header.magic = RVV_LEASE_CONTROL_MAGIC;
	page.header.abi_version = RVV_LEASE_CONTROL_ABI_VERSION;
	page.header.active = 1;
	page.header.target_tgid = config->target_tgid;
	page.header.max_workers = RVV_LEASE_CONTROL_WORKERS;
	page.header.loader_pid = (__u32)getpid();
	page.header.session_id = config->user_session_id;
	page.header.policy_epoch = config->user_policy_epoch;
	err = bpf_map__update_elem(skel->maps.user_worker_map, &key,
				   sizeof(key), &page, sizeof(page), BPF_ANY);
	if (err)
		return err;
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
	__u32 key = 0;

	if (!pinned || !skel)
		return;
	if (!bpf_map__lookup_elem(skel->maps.user_worker_map, &key, sizeof(key),
				  &page, sizeof(page), 0)) {
		page.header.active = 0;
		bpf_map__update_elem(skel->maps.user_worker_map, &key, sizeof(key),
				     &page, sizeof(page), BPF_ANY);
	}
	bpf_map__unpin(skel->maps.user_worker_map, worker_path);
	bpf_map__unpin(skel->maps.config_map, config_path);
	rmdir(dir);
}

static void print_worker_page(struct scx_llm_pin_bpf *skel)
{
	struct rvv_lease_worker_page page = {};
	__u32 key = 0;

	if (bpf_map__lookup_elem(skel->maps.user_worker_map, &key, sizeof(key),
				 &page, sizeof(page), 0) || !page.header.active)
		return;
	printf("\n=== userspace lease workers ===\n");
	printf("active=%u tgid=%u session=%llu epoch=%llu abi=%u\n",
	       page.header.active, page.header.target_tgid,
	       (unsigned long long)page.header.session_id,
	       (unsigned long long)page.header.policy_epoch,
	       page.header.abi_version);
	for (__u32 ith = 0; ith < RVV_LEASE_CONTROL_WORKERS; ith++) {
		const struct rvv_lease_worker_slot *slot = &page.workers[ith];

		if (!slot->sequence || !slot->tid)
			continue;
		printf("lease_worker ith=%u tid=%u tgid=%u n_threads=%u session=%llu seq=%u\n",
		       slot->ith, slot->tid, slot->tgid, slot->n_threads,
		       (unsigned long long)slot->session_id, slot->sequence);
	}
	fflush(stdout);
}

// 解析 CPU 范围字符串（如 "0-3" 或 "0,2,4"）
static __u32 parse_cpu_mask(const char *str)
{
	__u32 mask = 0;
	char *token, *tmp = strdup(str);
	char *saveptr;

	if (!tmp)
		return 0;

	token = strtok_r(tmp, ",", &saveptr);
	while (token) {
		int start, end;
		if (sscanf(token, "%d-%d", &start, &end) == 2) {
			// 范围格式：0-3
			for (int i = start; i <= end && i < 32; i++)
				mask |= (1U << i);
		} else if (sscanf(token, "%d", &start) == 1) {
			// 单个 CPU：0
			if (start < 32)
				mask |= (1U << start);
		}
		token = strtok_r(NULL, ",", &saveptr);
	}

	free(tmp);
	return mask;
}

static const char *strategy_name(__u32 strategy)
{
	switch (strategy) {
	case LLM_STRATEGY_GLOBAL:
		return "global";
	case LLM_STRATEGY_PIN:
		return "pin";
	case LLM_STRATEGY_LONG_SLICE:
		return "long_slice";
	case LLM_STRATEGY_LOCAL_DSQ:
		return "local_dsq";
	case LLM_STRATEGY_RVV_DIRTY:
		return "rvv_dirty";
	case LLM_STRATEGY_RVV_DIRTY_LOCAL:
		return "rvv_dirty_local";
	default:
		return "unknown";
	}
}

static int parse_strategy(const char *str, __u32 *strategy)
{
	if (!strcmp(str, "global") || !strcmp(str, "scx_global")) {
		*strategy = LLM_STRATEGY_GLOBAL;
		return 0;
	}
	if (!strcmp(str, "pin") || !strcmp(str, "llm_pin")) {
		*strategy = LLM_STRATEGY_PIN;
		return 0;
	}
	if (!strcmp(str, "long_slice")) {
		*strategy = LLM_STRATEGY_LONG_SLICE;
		return 0;
	}
	if (!strcmp(str, "local_dsq")) {
		*strategy = LLM_STRATEGY_LOCAL_DSQ;
		return 0;
	}
	if (!strcmp(str, "rvv_dirty") || !strcmp(str, "dirty")) {
		*strategy = LLM_STRATEGY_RVV_DIRTY;
		return 0;
	}
	if (!strcmp(str, "rvv_dirty_local") || !strcmp(str, "dirty_local")) {
		*strategy = LLM_STRATEGY_RVV_DIRTY_LOCAL;
		return 0;
	}

	return -EINVAL;
}

static int set_tgid_threads_policy(__u32 tgid, int policy)
{
	char path[64];
	struct sched_param param = { .sched_priority = 0 };
	struct dirent *de;
	DIR *dir;
	int count = 0;
	int failed = 0;

	snprintf(path, sizeof(path), "/proc/%u/task", tgid);
	dir = opendir(path);
	if (!dir) {
		fprintf(stderr, "failed to open %s: %s\n", path, strerror(errno));
		return -errno;
	}

	while ((de = readdir(dir))) {
		char *end = NULL;
		long tid;

		errno = 0;
		tid = strtol(de->d_name, &end, 10);
		if (errno || !end || *end != '\0' || tid <= 0)
			continue;

		if (sched_setscheduler((pid_t)tid, policy, &param)) {
			fprintf(stderr, "failed to set tid %ld policy %d: %s\n",
				tid, policy, strerror(errno));
			failed++;
			continue;
		}
		count++;
	}

	closedir(dir);

	if (!count && failed)
		return -EPERM;

	return count;
}

static int set_tid_list_policy(const char *tid_list, int policy)
{
	struct sched_param param = { .sched_priority = 0 };
	char *tmp;
	char *saveptr = NULL;
	char *token;
	int count = 0;
	int failed = 0;

	if (!tid_list || !*tid_list)
		return -EINVAL;

	tmp = strdup(tid_list);
	if (!tmp)
		return -ENOMEM;

	for (token = strtok_r(tmp, ",", &saveptr); token;
	     token = strtok_r(NULL, ",", &saveptr)) {
		char *end = NULL;
		long tid;

		errno = 0;
		tid = strtol(token, &end, 10);
		if (errno || !end || *end != '\0' || tid <= 0) {
			fprintf(stderr, "invalid tid in --tid-list: %s\n", token);
			failed++;
			continue;
		}

		if (sched_setscheduler((pid_t)tid, policy, &param)) {
			fprintf(stderr, "failed to set tid %ld policy %d: %s\n",
				tid, policy, strerror(errno));
			failed++;
			continue;
		}
		count++;
	}

	free(tmp);

	if (!count && failed)
		return -EPERM;
	return count;
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
	printf("LLM global dispatches:  %llu\n", stats.llm_global_dispatches);
	printf("LLM local dispatches:   %llu\n", stats.llm_local_dispatches);
	printf("LLM long slices:        %llu\n", stats.llm_long_slices);
	printf("System dispatches:      %llu\n", stats.system_dispatches);
	printf("RVV dirty samples:      %llu\n", stats.rvv_dirty_samples);
	printf("RVV vector threads:     %llu\n", stats.rvv_vector_threads);
	printf("RVV vector dispatches:  %llu\n", stats.rvv_vector_dispatches);
	printf("RVV aggressive dispatches: %llu\n", stats.rvv_aggressive_dispatches);
	printf("RVV fallback dispatches:   %llu\n", stats.rvv_fallback_dispatches);
	printf("RVV nonvector dispatches:  %llu\n", stats.rvv_nonvector_llm_dispatches);
	printf("Protected kernel dispatches: %llu\n", stats.protected_kernel_dispatches);
	printf("Protected kernel preempts:   %llu\n", stats.protected_kernel_preempts);
	printf("RVV vector DSQ dispatches:   %llu\n", stats.rvv_vector_dsq_dispatches);
	printf("RVV vector DSQ consumes:     %llu\n", stats.rvv_vector_dsq_consumes);
	printf("RVV vector local dispatches: %llu\n", stats.rvv_vector_local_dispatches);
	printf("RVV vector window starts:    %llu\n", stats.rvv_vector_window_starts);
	printf("RVV vector slice clamps:     %llu\n", stats.rvv_vector_slice_clamps);
	printf("RVV vector identify barrier dispatches: %llu\n",
	       stats.rvv_vector_identify_barrier_dispatches);
	printf("User hint classification reads: %llu\n",
	       stats.user_hint_classification_reads);
	printf("User hint valid workers:       %llu\n",
	       stats.user_hint_valid_workers);
	printf("User hint observed workers:    %llu\n",
	       stats.user_hint_observed_workers);
	printf("User hint active workers:      %llu\n",
	       stats.user_hint_active_workers);
	printf("User hint active assignments:  %llu\n",
	       stats.user_hint_active_assignments);
	printf("User hint invalid/fallback:    %llu/%llu\n",
	       stats.user_hint_invalid_workers, stats.user_hint_fallbacks);
	fflush(stdout);
}

static void print_vector_cpu_assignments(struct scx_llm_pin_bpf *skel)
{
	struct rvv_task_class task_class;
	__s32 selected_cpu;
	__u32 key;
	__u32 next_key;
	int class_fd = bpf_map__fd(skel->maps.rvv_task_classes);
	int cpu_fd = bpf_map__fd(skel->maps.selected_cpu_map);
	int err;

	printf("\n=== RVV vector CPU assignments ===\n");
	err = bpf_map_get_next_key(class_fd, NULL, &next_key);
	while (!err) {
		key = next_key;
		if (!bpf_map_lookup_elem(class_fd, &key, &task_class) &&
		    task_class.is_vector) {
			selected_cpu = -1;
			bpf_map_lookup_elem(cpu_fd, &key, &selected_cpu);
			printf("tid=%u dirty_score=%u selected_cpu=%d user_ith=%d user_session=%llu\n",
			       key, task_class.dirty_score, selected_cpu,
			       task_class.user_ith_plus1 ?
			       (int)task_class.user_ith_plus1 - 1 : -1,
			       (unsigned long long)task_class.user_session_id);
		}
		err = bpf_map_get_next_key(class_fd, &key, &next_key);
	}
	fflush(stdout);
}

enum {
	OPT_USER_CONTROL = 1000,
	OPT_CONTROL_DIR,
	OPT_USER_CPU_MAP,
};

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage: %s [options]\n"
		"\n"
		"Options:\n"
		"  --tgid <pid>           Target process TGID (llama-server PID)\n"
		"  --strategy <name>      global|pin|long_slice|local_dsq|rvv_dirty|rvv_dirty_local (default: pin)\n"
		"  --llm-cpus <range>     CPU mask for LLM threads (e.g., 0-3 or 0,1,2,3)\n"
		"  --system-cpus <range>  CPU mask for system threads (e.g., 4-7)\n"
		"  --vector-cpus <range>  Aggressive CPUs for RVV dirty vector threads\n"
		"  --ordinary-cpus <range> Ordinary/fallback CPUs for RVV dirty vector threads\n"
		"  --slice-ms <ms>        LLM time slice in milliseconds (0 = kernel default)\n"
		"  --system-slice-ms <ms> System task time slice in milliseconds (0 = kernel default)\n"
		"  --vector-slice-ms <ms> RVV vector slice on aggressive CPUs (0 = LLM/default)\n"
		"  --ordinary-vector-slice-ms <ms> RVV vector slice on ordinary CPUs (0 = LLM/default)\n"
		"  --vector-local-window-ms <ms> RVV local fast-path window after vector DSQ consume (default: 500)\n"
		"  --dirty-threshold <n>  Dirty samples before marking vector thread (default: 2)\n"
		"  --fallback-every <n>   Every Nth vector CPU pick uses ordinary CPUs (0 = disabled)\n"
		"  --user-control <mode>  off|observe|active (default: off)\n"
		"  --control-dir <path>   Pin config/worker maps under this bpffs directory\n"
		"  --user-cpu-map <map>   auto or OpenMP ith0..7 CPU order (default: auto)\n"
		"  --llm-only            Do not switch all tasks; move target threads to SCHED_EXT only\n"
		"  --tid-list <list>     With --llm-only, move only comma-separated TIDs to SCHED_EXT\n"
		"  --duration <sec>       Run duration in seconds (0 = infinite, default: 0)\n"
		"  -h, --help             Show this help\n"
		"\n"
		"Example:\n"
		"  %s --tgid 1234 --strategy long_slice --llm-cpus 0-3 --system-cpus 4-7 --slice-ms 100\n"
		"\n",
		prog, prog);
}

int main(int argc, char **argv)
{
	struct scx_llm_pin_bpf *skel = NULL;
	struct bpf_link *link = NULL;
	struct llm_pin_config config = {0};
	const char *tid_list = NULL;
	const char *control_dir = RVV_LEASE_CONTROL_DEFAULT_DIR;
	char config_map_path[PATH_MAX] = {};
	char worker_map_path[PATH_MAX] = {};
	int duration = 0;
	int moved_threads = 0;
	bool control_maps_pinned = false;
	int err;
	__u32 key = 0;

	config.strategy = LLM_STRATEGY_PIN;
	config.llm_cpu_mask = 0xFF;
	config.system_cpu_mask = 0xFF;
	config.vector_cpu_mask = 0xFF;
	config.ordinary_cpu_mask = 0xFF;
	config.dirty_threshold = 2;
	config.vector_local_window_ns = 500 * NSEC_PER_MSEC;
	config.switch_all = 1;
	for (__u32 ith = 0; ith < RVV_LEASE_CONTROL_WORKERS; ith++)
		config.user_cpu_map[ith] = ith;

	static struct option long_options[] = {
		{"tgid",        required_argument, 0, 't'},
		{"strategy",    required_argument, 0, 'p'},
		{"llm-cpus",    required_argument, 0, 'l'},
		{"system-cpus", required_argument, 0, 's'},
		{"vector-cpus", required_argument, 0, 'v'},
		{"ordinary-cpus", required_argument, 0, 'O'},
		{"slice-ms",    required_argument, 0, 'm'},
		{"system-slice-ms", required_argument, 0, 'S'},
		{"vector-slice-ms", required_argument, 0, 'M'},
		{"ordinary-vector-slice-ms", required_argument, 0, 'N'},
		{"vector-local-window-ms", required_argument, 0, 'W'},
		{"dirty-threshold", required_argument, 0, 'D'},
		{"fallback-every", required_argument, 0, 'F'},
		{"user-control", required_argument, 0, OPT_USER_CONTROL},
		{"control-dir", required_argument, 0, OPT_CONTROL_DIR},
		{"user-cpu-map", required_argument, 0, OPT_USER_CPU_MAP},
		{"llm-only",    no_argument,       0, 'o'},
		{"tid-list",    required_argument, 0, 'T'},
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
		case 't':
			config.target_tgid = atoi(optarg);
			break;
		case 'p':
			if (parse_strategy(optarg, &config.strategy)) {
				fprintf(stderr, "Error: unknown strategy '%s'\n", optarg);
				usage(argv[0]);
				return 1;
			}
			break;
		case 'l':
			config.llm_cpu_mask = parse_cpu_mask(optarg);
			break;
		case 's':
			config.system_cpu_mask = parse_cpu_mask(optarg);
			break;
		case 'v':
			config.vector_cpu_mask = parse_cpu_mask(optarg);
			break;
		case 'O':
			config.ordinary_cpu_mask = parse_cpu_mask(optarg);
			break;
		case 'm':
			config.llm_slice_ns = strtoull(optarg, NULL, 10) * NSEC_PER_MSEC;
			break;
		case 'S':
			config.system_slice_ns = strtoull(optarg, NULL, 10) * NSEC_PER_MSEC;
			break;
		case 'M':
			config.vector_slice_ns = strtoull(optarg, NULL, 10) * NSEC_PER_MSEC;
			break;
		case 'N':
			config.ordinary_vector_slice_ns = strtoull(optarg, NULL, 10) * NSEC_PER_MSEC;
			break;
		case 'W':
			config.vector_local_window_ns = strtoull(optarg, NULL, 10) * NSEC_PER_MSEC;
			break;
		case 'D':
			config.dirty_threshold = strtoul(optarg, NULL, 10);
			break;
		case 'F':
			config.fallback_every = strtoul(optarg, NULL, 10);
			break;
		case OPT_USER_CONTROL:
			if (parse_control_mode(optarg, &config.user_control_mode)) {
				fprintf(stderr, "Error: invalid --user-control '%s'\n", optarg);
				return 1;
			}
			break;
		case OPT_CONTROL_DIR:
			control_dir = optarg;
			break;
		case OPT_USER_CPU_MAP:
			if (parse_cpu_order(optarg, config.user_cpu_map,
					    &config.user_cpu_map_enabled)) {
				fprintf(stderr, "Error: --user-cpu-map requires auto or eight unique CPUs 0..7\n");
				return 1;
			}
			break;
		case 'o':
			config.switch_all = 0;
			break;
		case 'T':
			tid_list = optarg;
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
	if (!config.target_tgid) {
		fprintf(stderr, "Error: --tgid is required\n\n");
		usage(argv[0]);
		return 1;
	}
	if (!config.llm_cpu_mask)
		config.llm_cpu_mask = 0xFF;

	if (!config.system_cpu_mask) {
		config.system_cpu_mask = 0xFF & ~config.llm_cpu_mask;  // 8核默认
		if (!config.system_cpu_mask)
			config.system_cpu_mask = 0xFF;
	}
	if (!config.vector_cpu_mask)
		config.vector_cpu_mask = config.llm_cpu_mask;
	if (!config.ordinary_cpu_mask)
		config.ordinary_cpu_mask = config.system_cpu_mask;
	if (!config.dirty_threshold)
		config.dirty_threshold = 2;
	if (config.user_control_mode != RVV_LEASE_CONTROL_OFF &&
	    config.strategy != LLM_STRATEGY_RVV_DIRTY &&
	    config.strategy != LLM_STRATEGY_RVV_DIRTY_LOCAL) {
		fprintf(stderr, "Error: userspace control requires an rvv_dirty strategy\n");
		return 1;
	}
	if (config.user_control_mode != RVV_LEASE_CONTROL_OFF &&
	    (!control_dir || control_dir[0] != '/')) {
		fprintf(stderr, "Error: --control-dir must be an absolute bpffs path\n");
		return 1;
	}

	if (config.strategy == LLM_STRATEGY_LONG_SLICE && !config.llm_slice_ns)
		config.llm_slice_ns = 100 * NSEC_PER_MSEC;
	if (config.strategy == LLM_STRATEGY_RVV_DIRTY ||
	    config.strategy == LLM_STRATEGY_RVV_DIRTY_LOCAL) {
		if (!config.vector_slice_ns)
			config.vector_slice_ns = 100 * NSEC_PER_MSEC;
		if (!config.ordinary_vector_slice_ns)
			config.ordinary_vector_slice_ns = config.llm_slice_ns;
	}

	config.enabled = 1;
	if (config.user_control_mode != RVV_LEASE_CONTROL_OFF) {
		config.user_session_id = new_control_session_id();
		config.user_policy_epoch = 1;
	}

	// 打印配置
	printf("scx_llm_pin configuration:\n");
	printf("  Target TGID:     %u\n", config.target_tgid);
	printf("  Strategy:        %s\n", strategy_name(config.strategy));
	printf("  LLM CPU mask:    0x%02x\n", config.llm_cpu_mask);
	printf("  System CPU mask: 0x%02x\n", config.system_cpu_mask);
	printf("  Vector CPU mask: 0x%02x\n", config.vector_cpu_mask);
	printf("  Ordinary CPU mask: 0x%02x\n", config.ordinary_cpu_mask);
	printf("  LLM slice:       %llums\n",
	       (unsigned long long)(config.llm_slice_ns / NSEC_PER_MSEC));
	printf("  System slice:    %llums\n",
	       (unsigned long long)(config.system_slice_ns / NSEC_PER_MSEC));
	printf("  Vector slice:    %llums\n",
	       (unsigned long long)(config.vector_slice_ns / NSEC_PER_MSEC));
	printf("  Ordinary vector slice: %llums\n",
	       (unsigned long long)(config.ordinary_vector_slice_ns / NSEC_PER_MSEC));
	printf("  Vector local window: %llums\n",
	       (unsigned long long)(config.vector_local_window_ns / NSEC_PER_MSEC));
	printf("  Dirty threshold: %u\n", config.dirty_threshold);
	printf("  Fallback every:  %u\n", config.fallback_every);
	printf("  User control:    %s\n", control_mode_name(config.user_control_mode));
	if (config.user_control_mode != RVV_LEASE_CONTROL_OFF) {
		printf("  Control dir:     %s\n", control_dir);
		printf("  Control session: %llu\n",
		       (unsigned long long)config.user_session_id);
		if (config.user_cpu_map_enabled)
			printf("  User CPU map:    %u,%u,%u,%u,%u,%u,%u,%u\n",
			       config.user_cpu_map[0], config.user_cpu_map[1],
			       config.user_cpu_map[2], config.user_cpu_map[3],
			       config.user_cpu_map[4], config.user_cpu_map[5],
			       config.user_cpu_map[6], config.user_cpu_map[7]);
		else
			printf("  User CPU map:    auto\n");
	}
	printf("  Switch all:      %s\n", config.switch_all ? "yes" : "no");
	if (tid_list)
		printf("  TID list:        %s\n", tid_list);
	printf("  Duration:        %s\n", duration ? "limited" : "infinite");
	printf("\n");

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
	if (config.user_control_mode != RVV_LEASE_CONTROL_OFF) {
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
	}

	// 附加 struct_ops
	link = bpf_map__attach_struct_ops(skel->maps.scx_llm_pin_ops);
	if (!link) {
		err = -errno;
		fprintf(stderr, "failed to attach sched_ext struct_ops: %d\n", err);
		goto cleanup;
	}

	if (!config.switch_all) {
		if (tid_list)
			err = set_tid_list_policy(tid_list, SCHED_EXT);
		else
			err = set_tgid_threads_policy(config.target_tgid, SCHED_EXT);
		if (err < 0) {
			fprintf(stderr, "failed to move target threads to SCHED_EXT: %d\n", err);
			goto cleanup;
		}
		moved_threads = err;
		printf("Moved %d target threads to SCHED_EXT\n", moved_threads);
	}

	printf("scx_llm_pin attached successfully\n");
	printf("Press Ctrl-C to detach\n\n");
	fflush(stdout);

	// 运行主循环
	if (duration > 0) {
		for (int i = 0; i < duration && !exiting; i++) {
			sleep(1);
			if (i > 0 && i % 10 == 0) {
				print_stats(skel);
				print_worker_page(skel);
			}
		}
	} else {
		while (!exiting) {
			sleep(5);
			print_stats(skel);
			print_worker_page(skel);
		}
	}

	// 打印最终统计
	print_stats(skel);
	print_vector_cpu_assignments(skel);
	print_worker_page(skel);

cleanup:
	if (!config.switch_all && moved_threads > 0) {
		if (tid_list)
			set_tid_list_policy(tid_list, SCHED_NORMAL);
		else
			set_tgid_threads_policy(config.target_tgid, SCHED_NORMAL);
	}
	bpf_link__destroy(link);
	deactivate_and_unpin_control_maps(skel, control_dir, config_map_path,
					  worker_map_path, control_maps_pinned);
	scx_llm_pin_bpf__destroy(skel);
	printf("\nscx_llm_pin detached\n");
	return err < 0 ? -err : 0;
}
