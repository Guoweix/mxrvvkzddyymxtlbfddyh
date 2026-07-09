/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */
#ifndef __RVV_SCHED_COST_H
#define __RVV_SCHED_COST_H

#define RVV_COMM_LEN		16
#define RVV_MAX_CPUS		128
#define RVV_HIST_BUCKETS	32

#define RVV_SR_VS		0x00000600UL
#define RVV_SR_VS_OFF		0x00000000UL
#define RVV_SR_VS_INITIAL	0x00000200UL
#define RVV_SR_VS_CLEAN		0x00000400UL
#define RVV_SR_VS_DIRTY		0x00000600UL

enum rvv_hist_kind {
	RVV_HIST_OFFCPU = 0,
	RVV_HIST_WAKE_TO_RUN = 1,
	RVV_HIST_SWITCH_INTERVAL = 2,
	RVV_HIST_MAX = 3,
};

enum rvv_vs_bucket {
	RVV_VS_OFF_BUCKET = 0,
	RVV_VS_INITIAL_BUCKET = 1,
	RVV_VS_CLEAN_BUCKET = 2,
	RVV_VS_DIRTY_BUCKET = 3,
	RVV_VS_UNKNOWN_BUCKET = 4,
	RVV_VS_BUCKETS = 5,
};

struct rvv_config {
	unsigned int target_tgid;
	unsigned int target_tid;
	unsigned int sample_vs;
	unsigned int reserved;
};

struct rvv_task_key {
	unsigned int tgid;
	unsigned int tid;
};

struct rvv_task_ts {
	unsigned long long wake_ts;
	unsigned long long offcpu_ts;
	unsigned long long last_switch_ts;
};

struct rvv_hist {
	unsigned long long buckets[RVV_HIST_BUCKETS];
};

struct rvv_global_stats {
	unsigned long long switch_events;
	unsigned long long switches_in;
	unsigned long long switches_out;
	unsigned long long wakeups;
	unsigned long long wake_to_run_events;
	unsigned long long wake_to_run_total_ns;
	unsigned long long wake_to_run_max_ns;
	unsigned long long offcpu_events;
	unsigned long long offcpu_total_ns;
	unsigned long long offcpu_max_ns;
	unsigned long long switch_interval_events;
	unsigned long long switch_interval_total_ns;
	unsigned long long switch_interval_max_ns;
	unsigned long long v_save_candidates;
	unsigned long long v_restore_candidates;
	unsigned long long prev_vs_samples[RVV_VS_BUCKETS];
	unsigned long long next_vs_samples[RVV_VS_BUCKETS];
	unsigned long long lost_ts_updates;
};

struct rvv_thread_stats {
	unsigned int tgid;
	unsigned int tid;
	char comm[RVV_COMM_LEN];
	unsigned long long switches_in;
	unsigned long long switches_out;
	unsigned long long wakeups;
	unsigned long long wake_to_run_events;
	unsigned long long wake_to_run_total_ns;
	unsigned long long wake_to_run_max_ns;
	unsigned long long offcpu_events;
	unsigned long long offcpu_total_ns;
	unsigned long long offcpu_max_ns;
	unsigned long long switch_interval_events;
	unsigned long long switch_interval_total_ns;
	unsigned long long switch_interval_max_ns;
	unsigned long long v_save_candidates;
	unsigned long long v_restore_candidates;
	unsigned long long prev_vs_samples[RVV_VS_BUCKETS];
	unsigned long long next_vs_samples[RVV_VS_BUCKETS];
};

struct rvv_cpu_stats {
	unsigned int cpu;
	unsigned long long switch_events;
	unsigned long long switches_in;
	unsigned long long switches_out;
	unsigned long long wakeups;
	unsigned long long v_save_candidates;
	unsigned long long v_restore_candidates;
};

#endif
