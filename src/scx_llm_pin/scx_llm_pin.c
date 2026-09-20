// SPDX-License-Identifier: GPL-2.0
/*
 * scx_llm_pin - User-space loader
 *
 * 用法:
 *   scx_llm_pin --tgid <pid> --strategy rvv_dirty [--duration 60]
 */
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
#include "scx_llm_pin.skel.h"

#define NSEC_PER_MSEC 1000000ULL
#define NSEC_PER_USEC 1000ULL
#ifndef BPF_F_SLEEPABLE
#define BPF_F_SLEEPABLE (1U << 4)
#endif

static volatile sig_atomic_t exiting = 0;

struct gang_state_snapshot {
	__u32 lock;
	__u32 active_team_plus1;
	__u32 cycle_slot;
	__u32 phase_signature;
	__u32 engaged;
	__u32 padding;
	__u64 quantum_deadline_ns;
	__u64 generation;
};

struct gang_sample_stats {
	__u64 interval_us;
	__u64 samples;
	__u64 engaged_samples;
	__u64 full_gang_samples;
	__u64 mixed_team_samples;
	__u64 active_team_slots;
	__u64 wrong_team_slots;
	__u64 idle_slots;
	__u64 unstable_samples;
	__u64 read_errors;
};

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

static const char *team_gang_mode_name(__u32 mode)
{
	switch (mode) {
	case LLM_TEAM_GANG_OFF:
		return "off";
	case LLM_TEAM_GANG_OBSERVE:
		return "observe";
	case LLM_TEAM_GANG_ALWAYS:
		return "always";
	case LLM_TEAM_GANG_PREFILL_LEASE:
		return "prefill-lease";
	default:
		return "unknown";
	}
}

static int parse_team_gang_mode(const char *str, __u32 *mode)
{
	if (!strcmp(str, "off"))
		*mode = LLM_TEAM_GANG_OFF;
	else if (!strcmp(str, "observe"))
		*mode = LLM_TEAM_GANG_OBSERVE;
	else if (!strcmp(str, "always"))
		*mode = LLM_TEAM_GANG_ALWAYS;
	else if (!strcmp(str, "prefill-lease"))
		*mode = LLM_TEAM_GANG_PREFILL_LEASE;
	else
		return -EINVAL;
	return 0;
}

static int parse_vector_local_wakeup_mode(const char *str, __u32 *mode)
{
	if (!strcmp(str, "team"))
		*mode = LLM_VECTOR_LOCAL_WAKEUP_TEAM;
	else if (!strcmp(str, "off"))
		*mode = LLM_VECTOR_LOCAL_WAKEUP_OFF;
	else if (!strcmp(str, "exclusive"))
		*mode = LLM_VECTOR_LOCAL_WAKEUP_EXCLUSIVE;
	else
		return -EINVAL;
	return 0;
}

static const char *vector_local_wakeup_mode_name(__u32 mode)
{
	switch (mode) {
	case LLM_VECTOR_LOCAL_WAKEUP_TEAM:
		return "team";
	case LLM_VECTOR_LOCAL_WAKEUP_OFF:
		return "off";
	case LLM_VECTOR_LOCAL_WAKEUP_EXCLUSIVE:
		return "exclusive";
	default:
		return "unknown";
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
	return team == 0 ? config->target_tgid : config->target_tgid2;
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
		printf("team_hint team=%u phase=%s phase_epoch=%llu workers=%u prompt=%u decode=%u begin_ns=%llu expires_ns=%llu seq=%u\n",
		       key, phase_name(page.team_hint.phase),
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
			printf("lease_worker team=%u ith=%u tid=%u tgid=%u n_threads=%u session=%llu seq=%u\n",
			       key, slot->ith, slot->tid, slot->tgid, slot->n_threads,
			       (unsigned long long)slot->session_id, slot->sequence);
		}
	}
	fflush(stdout);
}

// 解析 CPU 范围字符串（如 "0-3" 或 "0,2,4"）
static const char *strategy_name(__u32 strategy)
{
	switch (strategy) {
	case LLM_STRATEGY_GLOBAL:
		return "global";
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
	printf("System dispatches:      %llu\n", stats.system_dispatches);
	printf("RVV dirty samples:      %llu\n", stats.rvv_dirty_samples);
	printf("RVV vector threads:     %llu\n", stats.rvv_vector_threads);
	printf("RVV vector threads by team: %llu/%llu\n",
	       stats.rvv_vector_threads_team0, stats.rvv_vector_threads_team1);
	printf("RVV vector dispatches:  %llu\n", stats.rvv_vector_dispatches);
	printf("RVV nonvector dispatches:  %llu\n", stats.rvv_nonvector_llm_dispatches);
	printf("Protected kernel dispatches: %llu\n", stats.protected_kernel_dispatches);
	printf("Protected kernel preempts:   %llu\n", stats.protected_kernel_preempts);
	printf("RVV vector DSQ dispatches:   %llu\n", stats.rvv_vector_dsq_dispatches);
	printf("RVV vector DSQ consumes:     %llu\n", stats.rvv_vector_dsq_consumes);
	printf("RVV vector DSQ dispatches by team: %llu/%llu\n",
	       stats.rvv_vector_dsq_dispatches_team0,
	       stats.rvv_vector_dsq_dispatches_team1);
	printf("RVV vector DSQ consumes by team:   %llu/%llu\n",
	       stats.rvv_vector_dsq_consumes_team0,
	       stats.rvv_vector_dsq_consumes_team1);
	printf("RVV vector local dispatches: %llu\n", stats.rvv_vector_local_dispatches);
	printf("RVV vector window starts:    %llu\n", stats.rvv_vector_window_starts);
	printf("RVV exclusive lease handoffs/denials: %llu/%llu\n",
	       stats.vector_exclusive_lease_handoffs,
	       stats.vector_exclusive_lease_denials);
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
	printf("User preference DSQ consumes:       %llu\n",
	       stats.user_preference_dsq_consumes);
	printf("User preference empty fallbacks:    %llu\n",
	       stats.user_preference_empty_fallbacks);
	printf("User hint primary workers/misses:    %llu/%llu\n",
	       stats.user_hint_primary_workers, stats.user_hint_member_misses);
	printf("User hint deferred/excluded/demoted: %llu/%llu/%llu\n",
	       stats.user_hint_deferred_tasks, stats.user_hint_excluded_tasks,
	       stats.user_hint_demotions);
	printf("Phase hint reads valid/invalid:      %llu/%llu/%llu\n",
	       stats.phase_hint_reads, stats.phase_hint_valid, stats.phase_hint_invalid);
	printf("Phase hint idle/prefill/decode/mixed: %llu/%llu/%llu/%llu\n",
	       stats.phase_hint_idle, stats.phase_hint_prefill,
	       stats.phase_hint_decode, stats.phase_hint_mixed);
	printf("Gang engagements/quanta/switches:    %llu/%llu/%llu\n",
	       stats.gang_engagements, stats.gang_quanta, stats.gang_team_switches);
	printf("Gang kicks/vector enqueues:          %llu/%llu\n",
	       stats.gang_cpu_kicks, stats.gang_vector_enqueues);
	printf("Gang consumes selected/empty/fallback: %llu/%llu/%llu\n",
	       stats.gang_selected_consumes, stats.gang_empty_fallbacks,
	       stats.gang_fallback_consumes);
	printf("Gang runs total/active/wrong:        %llu/%llu/%llu\n",
	       stats.gang_running_samples, stats.gang_active_team_runs,
	       stats.gang_wrong_team_runs);
	printf("Phase lease engagements/preferred/empty/fallback: %llu/%llu/%llu/%llu\n",
	       stats.phase_lease_engagements,
	       stats.phase_lease_preferred_consumes,
	       stats.phase_lease_empty_fallbacks,
	       stats.phase_lease_fallback_consumes);
	printf("Phase lease safety forced/miss/stale: %llu/%llu/%llu\n",
	       stats.phase_lease_safety_forced,
	       stats.phase_lease_safety_misses,
	       stats.phase_lease_pending_stale);
	printf("Phase lease pending max/ge4s:          %lluns/%llu\n",
	       stats.phase_lease_pending_max_ns,
	       stats.phase_lease_pending_ge_4s);
	printf("Phase lease runs total/preferred/other: %llu/%llu/%llu\n",
	       stats.phase_lease_running_samples,
	       stats.phase_lease_preferred_runs,
	       stats.phase_lease_other_runs);
	printf("Phase lease wakeup candidates/preempts:  %llu/%llu\n",
	       stats.phase_lease_wakeup_candidates,
	       stats.phase_lease_wakeup_preempts);
	printf("SCX_GANG_STATS rvv_team0=%llu rvv_team1=%llu hint_valid_workers=%llu hint_active_workers=%llu hint_fallbacks=%llu hint_deferred=%llu hint_excluded=%llu hint_demotions=%llu phase_reads=%llu phase_valid=%llu phase_invalid=%llu phase_idle=%llu phase_prefill=%llu phase_decode=%llu phase_mixed=%llu gang_engagements=%llu gang_quanta=%llu gang_switches=%llu gang_kicks=%llu gang_enqueues=%llu gang_selected=%llu gang_empty=%llu gang_fallback=%llu gang_runs=%llu gang_active_runs=%llu gang_wrong_runs=%llu phase_lease_engagements=%llu phase_lease_preferred=%llu phase_lease_empty=%llu phase_lease_fallback=%llu phase_lease_safety_forced=%llu phase_lease_safety_misses=%llu phase_lease_pending_stale=%llu phase_lease_pending_max_ns=%llu phase_lease_pending_ge_4s=%llu phase_lease_runs=%llu phase_lease_preferred_runs=%llu phase_lease_other_runs=%llu phase_lease_wakeup_candidates=%llu phase_lease_wakeup_preempts=%llu vector_local_dispatches=%llu vector_window_starts=%llu exclusive_handoffs=%llu exclusive_denials=%llu dsq_dispatches=%llu dsq_consumes=%llu dsq_dispatches_team0=%llu dsq_dispatches_team1=%llu dsq_consumes_team0=%llu dsq_consumes_team1=%llu\n",
	       stats.rvv_vector_threads_team0, stats.rvv_vector_threads_team1,
	       stats.user_hint_valid_workers, stats.user_hint_active_workers,
	       stats.user_hint_fallbacks, stats.user_hint_deferred_tasks,
	       stats.user_hint_excluded_tasks, stats.user_hint_demotions,
	       stats.phase_hint_reads,
	       stats.phase_hint_valid, stats.phase_hint_invalid,
	       stats.phase_hint_idle, stats.phase_hint_prefill,
	       stats.phase_hint_decode, stats.phase_hint_mixed,
	       stats.gang_engagements, stats.gang_quanta,
	       stats.gang_team_switches, stats.gang_cpu_kicks,
	       stats.gang_vector_enqueues, stats.gang_selected_consumes,
	       stats.gang_empty_fallbacks, stats.gang_fallback_consumes,
	       stats.gang_running_samples, stats.gang_active_team_runs,
	       stats.gang_wrong_team_runs,
	       stats.phase_lease_engagements,
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
	       stats.phase_lease_wakeup_preempts,
	       stats.rvv_vector_local_dispatches,
	       stats.rvv_vector_window_starts,
	       stats.vector_exclusive_lease_handoffs,
	       stats.vector_exclusive_lease_denials,
	       stats.rvv_vector_dsq_dispatches,
	       stats.rvv_vector_dsq_consumes,
	       stats.rvv_vector_dsq_dispatches_team0,
	       stats.rvv_vector_dsq_dispatches_team1,
	       stats.rvv_vector_dsq_consumes_team0,
	       stats.rvv_vector_dsq_consumes_team1);
	fflush(stdout);
}

static void sample_gang_placement(struct scx_llm_pin_bpf *skel,
				  struct gang_sample_stats *samples)
{
	struct gang_state_snapshot before = {};
	struct gang_state_snapshot after = {};
	__u32 key = 0;
	__u32 expected_slots = 0;
	__u32 active_slots = 0;
	__u32 wrong_slots = 0;
	__u32 idle_slots = 0;

	samples->samples++;
	if (bpf_map__lookup_elem(skel->maps.team_gang_state_map, &key,
				 sizeof(key), &before, sizeof(before), 0)) {
		samples->read_errors++;
		return;
	}
	if (!before.engaged || !before.active_team_plus1 ||
	    before.active_team_plus1 > RVV_LEASE_CONTROL_TEAMS)
		return;

	for (__u32 cpu = 0; cpu < RVV_LEASE_CONTROL_WORKERS; cpu++) {
		__u32 running_team = 0;

		expected_slots++;
		key = cpu;
		if (bpf_map__lookup_elem(skel->maps.running_team_by_cpu, &key,
					 sizeof(key), &running_team,
					 sizeof(running_team), 0)) {
			samples->read_errors++;
			return;
		}
		if (!running_team)
			idle_slots++;
		else if (running_team == before.active_team_plus1)
			active_slots++;
		else
			wrong_slots++;
	}

	key = 0;
	if (bpf_map__lookup_elem(skel->maps.team_gang_state_map, &key,
				 sizeof(key), &after, sizeof(after), 0)) {
		samples->read_errors++;
		return;
	}
	if (before.generation != after.generation ||
	    before.engaged != after.engaged ||
	    before.active_team_plus1 != after.active_team_plus1) {
		samples->unstable_samples++;
		return;
	}

	samples->engaged_samples++;
	samples->active_team_slots += active_slots;
	samples->wrong_team_slots += wrong_slots;
	samples->idle_slots += idle_slots;
	if (wrong_slots)
		samples->mixed_team_samples++;
	if (expected_slots && active_slots == expected_slots)
		samples->full_gang_samples++;
}

static void print_gang_samples(const struct gang_sample_stats *samples)
{
	printf("SCX_GANG_SAMPLES interval_us=%llu samples=%llu engaged=%llu full_gang=%llu mixed_team=%llu active_slots=%llu wrong_slots=%llu idle_slots=%llu unstable=%llu read_errors=%llu\n",
	       samples->interval_us, samples->samples, samples->engaged_samples,
	       samples->full_gang_samples, samples->mixed_team_samples,
	       samples->active_team_slots, samples->wrong_team_slots,
	       samples->idle_slots, samples->unstable_samples,
	       samples->read_errors);
	fflush(stdout);
}

static void run_main_loop(struct scx_llm_pin_bpf *skel,
			  int duration, __u64 gang_sample_us,
			  struct gang_sample_stats *gang_samples)
{
	__u64 start_ns = monotonic_ns();
	__u64 report_ns = start_ns + 10ULL * 1000000000ULL;
	__u64 deadline_ns = duration > 0 ?
		start_ns + (__u64)duration * 1000000000ULL : 0;
	struct timespec delay = {};

	if (!gang_sample_us) {
		while (!exiting && (!deadline_ns || monotonic_ns() < deadline_ns)) {
			sleep(duration > 0 ? 1 : 5);
			if (monotonic_ns() >= report_ns) {
				print_stats(skel);
				print_worker_page(skel);
				report_ns = monotonic_ns() + 10ULL * 1000000000ULL;
			}
		}
		return;
	}

	delay.tv_sec = gang_sample_us / 1000000ULL;
	delay.tv_nsec = (gang_sample_us % 1000000ULL) * NSEC_PER_USEC;
	while (!exiting && (!deadline_ns || monotonic_ns() < deadline_ns)) {
		sample_gang_placement(skel, gang_samples);
		if (monotonic_ns() >= report_ns) {
			print_stats(skel);
			print_worker_page(skel);
			print_gang_samples(gang_samples);
			report_ns = monotonic_ns() + 10ULL * 1000000000ULL;
		}
		nanosleep(&delay, NULL);
	}
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
			printf("tid=%u team=%d dirty_score=%u selected_cpu=%d user_ith=%d user_session=%llu\n",
			       key, task_class.team_id_plus1 ?
			       (int)task_class.team_id_plus1 - 1 : -1,
			       task_class.dirty_score, selected_cpu,
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
	OPT_TGID2,
	OPT_TEAM_GANG,
	OPT_GANG_QUANTUM_US,
	OPT_PHASE_HINT_TIMEOUT_MS,
	OPT_PREFILL_STARVATION_MS,
	OPT_GANG_SAMPLE_US,
	OPT_VECTOR_LOCAL_WAKEUP,
	OPT_PHASE_WAKEUP_PREEMPT,
};

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage: %s [options]\n"
		"\n"
		"Options:\n"
		"  --tgid <pid>           Target process TGID (llama-server PID)\n"
		"  --tgid2 <pid>          Optional second llama-server TGID\n"
		"  --strategy <name>      global|rvv_dirty|rvv_dirty_local (default: global)\n"
		"  --vector-slice-ms <ms> RVV vector worker slice (default: 100)\n"
		"  --vector-local-window-ms <ms> RVV local fast-path window after vector DSQ consume (default: 500)\n"
		"  --vector-local-wakeup <mode> team|off|exclusive (default: team)\n"
		"  --dirty-threshold <n>  Dirty samples before marking vector thread (default: 2)\n"
		"  --user-control <mode>  off|observe|active (default: off)\n"
		"  --control-dir <path>   Pin config/worker maps under this bpffs directory\n"
		"  --user-cpu-map <map>   auto or OpenMP ith0..7 CPU order (default: auto)\n"
		"  --team-gang <mode>     off|observe|always|prefill-lease (default: off)\n"
		"  --gang-quantum-us <us> Shared team gang quantum (default: 5000)\n"
		"  --phase-hint-timeout-ms <ms> Reject stale phase hints (default: 300000)\n"
		"  --prefill-starvation-ms <ms> V DSQ safety wait for prefill-lease (default: 1000)\n"
		"  --phase-wakeup-preempt <on|off> Preferred wakeup targeted preemption (default: on)\n"
		"  --gang-sample-us <us> Sample running teams for diagnostics (0=off, min=1000)\n"
		"  --duration <sec>       Run duration in seconds (0 = infinite, default: 0)\n"
		"  -h, --help             Show this help\n"
		"\n"
		"Example:\n"
		"  %s --tgid 1234 --strategy rvv_dirty --vector-slice-ms 100\n"
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
	__u64 gang_sample_us = 0;
	struct gang_sample_stats gang_samples = {};
	bool control_maps_pinned = false;
	int err;
	__u32 key = 0;

	config.strategy = LLM_STRATEGY_GLOBAL;
	config.dirty_threshold = 2;
	config.vector_local_window_ns = 500 * NSEC_PER_MSEC;
	config.vector_local_wakeup_mode = LLM_VECTOR_LOCAL_WAKEUP_TEAM;
	config.phase_wakeup_preempt_enabled = 1;
	config.team_gang_mode = LLM_TEAM_GANG_OFF;
	config.gang_quantum_ns = 5000 * NSEC_PER_USEC;
	config.phase_hint_timeout_ns = 300000 * NSEC_PER_MSEC;
	config.prefill_starvation_ns = 1000 * NSEC_PER_MSEC;
	for (__u32 ith = 0; ith < RVV_LEASE_CONTROL_WORKERS; ith++)
		config.user_cpu_map[ith] = ith;

	static struct option long_options[] = {
		{"tgid",        required_argument, 0, 't'},
		{"tgid2",       required_argument, 0, OPT_TGID2},
		{"strategy",    required_argument, 0, 'p'},
		{"vector-slice-ms", required_argument, 0, 'M'},
		{"vector-local-window-ms", required_argument, 0, 'W'},
		{"vector-local-wakeup", required_argument, 0, OPT_VECTOR_LOCAL_WAKEUP},
		{"dirty-threshold", required_argument, 0, 'D'},
		{"user-control", required_argument, 0, OPT_USER_CONTROL},
		{"control-dir", required_argument, 0, OPT_CONTROL_DIR},
		{"user-cpu-map", required_argument, 0, OPT_USER_CPU_MAP},
		{"team-gang", required_argument, 0, OPT_TEAM_GANG},
		{"gang-quantum-us", required_argument, 0, OPT_GANG_QUANTUM_US},
		{"phase-hint-timeout-ms", required_argument, 0, OPT_PHASE_HINT_TIMEOUT_MS},
		{"prefill-starvation-ms", required_argument, 0, OPT_PREFILL_STARVATION_MS},
		{"phase-wakeup-preempt", required_argument, 0, OPT_PHASE_WAKEUP_PREEMPT},
		{"gang-sample-us", required_argument, 0, OPT_GANG_SAMPLE_US},
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
		case OPT_TGID2:
			config.target_tgid2 = atoi(optarg);
			break;
		case 'p':
			if (parse_strategy(optarg, &config.strategy)) {
				fprintf(stderr, "Error: unknown strategy '%s'\n", optarg);
				usage(argv[0]);
				return 1;
			}
			break;
		case 'M':
			config.vector_slice_ns = strtoull(optarg, NULL, 10) * NSEC_PER_MSEC;
			break;
		case 'W':
			config.vector_local_window_ns = strtoull(optarg, NULL, 10) * NSEC_PER_MSEC;
			break;
		case OPT_VECTOR_LOCAL_WAKEUP:
			if (parse_vector_local_wakeup_mode(
					optarg, &config.vector_local_wakeup_mode)) {
				fprintf(stderr, "Error: --vector-local-wakeup requires team|off|exclusive\n");
				return 1;
			}
			break;
		case 'D':
			config.dirty_threshold = strtoul(optarg, NULL, 10);
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
		case OPT_TEAM_GANG:
			if (parse_team_gang_mode(optarg, &config.team_gang_mode)) {
				fprintf(stderr, "Error: invalid --team-gang '%s'\n", optarg);
				return 1;
			}
			break;
		case OPT_GANG_QUANTUM_US:
			config.gang_quantum_ns = strtoull(optarg, NULL, 10) * NSEC_PER_USEC;
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
		case OPT_GANG_SAMPLE_US:
			gang_sample_us = strtoull(optarg, NULL, 10);
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
	if (config.target_tgid2 == config.target_tgid) {
		fprintf(stderr, "Error: --tgid2 must differ from --tgid\n");
		return 1;
	}
	if (!config.dirty_threshold)
		config.dirty_threshold = 2;
	if (config.user_control_mode != RVV_LEASE_CONTROL_OFF &&
	    config.strategy != LLM_STRATEGY_RVV_DIRTY &&
	    config.strategy != LLM_STRATEGY_RVV_DIRTY_LOCAL) {
		fprintf(stderr, "Error: userspace control requires an rvv_dirty strategy\n");
		return 1;
	}
	if (config.team_gang_mode != LLM_TEAM_GANG_OFF && !config.target_tgid2) {
		fprintf(stderr, "Error: --team-gang requires --tgid2\n");
		return 1;
	}
	if (config.team_gang_mode != LLM_TEAM_GANG_OFF &&
	    config.user_control_mode == RVV_LEASE_CONTROL_OFF) {
		fprintf(stderr, "Error: --team-gang requires userspace control\n");
		return 1;
	}
	if ((config.team_gang_mode == LLM_TEAM_GANG_ALWAYS ||
	     config.team_gang_mode == LLM_TEAM_GANG_PREFILL_LEASE) &&
	    config.user_control_mode != RVV_LEASE_CONTROL_ACTIVE) {
		fprintf(stderr, "Error: active phase modes require --user-control active\n");
		return 1;
	}
	if (config.gang_quantum_ns < 500 * NSEC_PER_USEC ||
	    config.gang_quantum_ns > 50000 * NSEC_PER_USEC) {
		fprintf(stderr, "Error: --gang-quantum-us must be in 500..50000\n");
		return 1;
	}
	if (config.prefill_starvation_ns < 100 * NSEC_PER_MSEC ||
	    config.prefill_starvation_ns > 4000 * NSEC_PER_MSEC) {
		fprintf(stderr, "Error: --prefill-starvation-ms must be in 100..4000\n");
		return 1;
	}
	if (config.vector_local_wakeup_mode >
	    LLM_VECTOR_LOCAL_WAKEUP_EXCLUSIVE) {
		fprintf(stderr, "Error: invalid vector local wakeup mode\n");
		return 1;
	}
	if (gang_sample_us && (gang_sample_us < 1000 || gang_sample_us > 1000000)) {
		fprintf(stderr, "Error: --gang-sample-us must be 0 or in 1000..1000000\n");
		return 1;
	}
	if (gang_sample_us &&
	    (config.team_gang_mode == LLM_TEAM_GANG_OFF ||
	     config.team_gang_mode == LLM_TEAM_GANG_PREFILL_LEASE)) {
		fprintf(stderr, "Error: --gang-sample-us requires --team-gang observe|always\n");
		return 1;
	}
	if (config.user_control_mode != RVV_LEASE_CONTROL_OFF &&
	    (!control_dir || control_dir[0] != '/')) {
		fprintf(stderr, "Error: --control-dir must be an absolute bpffs path\n");
		return 1;
	}

	if (config.strategy == LLM_STRATEGY_RVV_DIRTY ||
	    config.strategy == LLM_STRATEGY_RVV_DIRTY_LOCAL) {
		if (!config.vector_slice_ns)
			config.vector_slice_ns = 100 * NSEC_PER_MSEC;
	}

	config.enabled = 1;
	if (config.user_control_mode != RVV_LEASE_CONTROL_OFF) {
		config.user_session_id = new_control_session_id();
		config.user_policy_epoch = 1;
	}

	// 打印配置
	printf("scx_llm_pin configuration:\n");
	printf("  Target TGID:     %u\n", config.target_tgid);
	if (config.target_tgid2)
		printf("  Target TGID 2:   %u\n", config.target_tgid2);
	printf("  Strategy:        %s\n", strategy_name(config.strategy));
	printf("  Vector slice:    %llums\n",
	       (unsigned long long)(config.vector_slice_ns / NSEC_PER_MSEC));
	printf("  Vector local window: %llums\n",
	       (unsigned long long)(config.vector_local_window_ns / NSEC_PER_MSEC));
	printf("  Vector local wakeup: %s\n",
	       vector_local_wakeup_mode_name(config.vector_local_wakeup_mode));
	printf("  Dirty threshold: %u\n", config.dirty_threshold);
	printf("  User control:    %s\n", control_mode_name(config.user_control_mode));
	printf("  User preference: off\n");
	printf("  Team gang:       %s\n", team_gang_mode_name(config.team_gang_mode));
	printf("  Gang quantum:    %lluus\n",
	       (unsigned long long)(config.gang_quantum_ns / NSEC_PER_USEC));
	printf("  Phase timeout:   %llums\n",
	       (unsigned long long)(config.phase_hint_timeout_ns / NSEC_PER_MSEC));
	printf("  Prefill starvation guard: %llums\n",
	       (unsigned long long)(config.prefill_starvation_ns / NSEC_PER_MSEC));
	printf("  Phase wakeup preempt: %s\n",
	       config.phase_wakeup_preempt_enabled ? "on" : "off");
	printf("  Gang sampler:    %lluus\n",
	       (unsigned long long)gang_sample_us);
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

	printf("scx_llm_pin attached successfully\n");
	printf("Press Ctrl-C to detach\n\n");
	fflush(stdout);

	gang_samples.interval_us = gang_sample_us;
	run_main_loop(skel, duration, gang_sample_us, &gang_samples);

	// 打印最终统计
	print_stats(skel);
	print_gang_samples(&gang_samples);
	print_vector_cpu_assignments(skel);
	print_worker_page(skel);

cleanup:
	bpf_link__destroy(link);
	deactivate_and_unpin_control_maps(skel, control_dir, config_map_path,
					  worker_map_path, control_maps_pinned);
	scx_llm_pin_bpf__destroy(skel);
	printf("\nscx_llm_pin detached\n");
	return err < 0 ? -err : 0;
}
