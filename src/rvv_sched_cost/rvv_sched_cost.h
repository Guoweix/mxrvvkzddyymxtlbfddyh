/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */
#ifndef __RVV_SCHED_COST_H
#define __RVV_SCHED_COST_H

#define RVV_COMM_LEN		16
#define RVV_MAX_CPUS		128
#define RVV_HIST_BUCKETS	32
#define RVV_STACK_DEPTH		16

#define RVV_SR_VS		0x00000600UL
#define RVV_SR_VS_OFF		0x00000000UL
#define RVV_SR_VS_INITIAL	0x00000200UL
#define RVV_SR_VS_CLEAN		0x00000400UL
#define RVV_SR_VS_DIRTY		0x00000600UL

#define RVV_TASK_RUNNING		0x00000000UL
#define RVV_TASK_INTERRUPTIBLE		0x00000001UL
#define RVV_TASK_UNINTERRUPTIBLE	0x00000002UL

enum rvv_hist_kind {
	RVV_HIST_OFFCPU = 0,
	RVV_HIST_WAKE_TO_RUN = 1,
	RVV_HIST_SWITCH_INTERVAL = 2,
	RVV_HIST_FUTEX_WAIT_BLOCKED = 3,
	RVV_HIST_MAX = 4,
};

enum rvv_futex_wait_ret_bucket {
	RVV_FUTEX_RET_WOKEN = 0,
	RVV_FUTEX_RET_EAGAIN = 1,
	RVV_FUTEX_RET_EINTR = 2,
	RVV_FUTEX_RET_ETIMEDOUT = 3,
	RVV_FUTEX_RET_OTHER = 4,
	RVV_FUTEX_RET_BUCKETS = 5,
};

enum rvv_vs_bucket {
	RVV_VS_OFF_BUCKET = 0,
	RVV_VS_INITIAL_BUCKET = 1,
	RVV_VS_CLEAN_BUCKET = 2,
	RVV_VS_DIRTY_BUCKET = 3,
	RVV_VS_UNKNOWN_BUCKET = 4,
	RVV_VS_BUCKETS = 5,
};

enum rvv_out_state_bucket {
	RVV_OUT_RUNNING_BUCKET = 0,
	RVV_OUT_PREEMPT_BUCKET = 1,
	RVV_OUT_SLEEPING_BUCKET = 2,
	RVV_OUT_UNINTERRUPTIBLE_BUCKET = 3,
	RVV_OUT_OTHER_BUCKET = 4,
	RVV_OUT_STATE_BUCKETS = 5,
};

struct rvv_config {
	unsigned int target_tgid;
	unsigned int target_tid;
	unsigned int sample_vs;
	unsigned int sample_switch_out;
	unsigned int sample_out_state;
	unsigned int sample_futex;
	unsigned int sample_futex_detail;
	unsigned int sample_futex_stack;
	unsigned int sample_futex_time;
	unsigned int rvv_only;
};

struct rvv_task_key {
	unsigned int tgid;
	unsigned int tid;
};

struct rvv_waker_key {
	unsigned int wakee_tgid;
	unsigned int wakee_tid;
	unsigned int waker_tgid;
	unsigned int waker_tid;
};

struct rvv_futex_addr_key {
	unsigned long long uaddr;
};

struct rvv_futex_tid_key {
	unsigned long long uaddr;
	unsigned int tgid;
	unsigned int tid;
};

struct rvv_futex_stack_key {
	unsigned long long uaddr;
	int stack_id;
	unsigned int kind;
};

struct rvv_futex_addr_hist_key {
	unsigned long long uaddr;
	unsigned int bucket;
};

struct rvv_futex_wait_inflight {
	unsigned long long start_ns;
	unsigned long long uaddr;
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
	unsigned long long switches_out_state[RVV_OUT_STATE_BUCKETS];
	unsigned long long v_save_out_state[RVV_OUT_STATE_BUCKETS];
	unsigned long long prev_vs_samples[RVV_VS_BUCKETS];
	unsigned long long next_vs_samples[RVV_VS_BUCKETS];
	unsigned long long lost_ts_updates;
	unsigned long long futex_syscalls;
	unsigned long long futex_wait_ops;
	unsigned long long futex_wake_ops;
	unsigned long long futex_requeue_ops;
	unsigned long long futex_wake_op_ops;
	unsigned long long futex_pi_ops;
	unsigned long long futex_waitv_syscalls;
	unsigned long long futex_other_ops;
	unsigned long long futex_wait_completed_ops;
	unsigned long long futex_wait_completed_total_ns;
	unsigned long long futex_wait_blocked_ops;
	unsigned long long futex_wait_blocked_total_ns;
	unsigned long long futex_wait_blocked_max_ns;
	unsigned long long futex_wait_ret[RVV_FUTEX_RET_BUCKETS];
	unsigned long long futex_wait_ret_total_ns[RVV_FUTEX_RET_BUCKETS];
	unsigned long long futex_wait_start_failures;
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
	unsigned long long switches_out_state[RVV_OUT_STATE_BUCKETS];
	unsigned long long v_save_out_state[RVV_OUT_STATE_BUCKETS];
	unsigned long long prev_vs_samples[RVV_VS_BUCKETS];
	unsigned long long next_vs_samples[RVV_VS_BUCKETS];
	unsigned long long futex_syscalls;
	unsigned long long futex_wait_ops;
	unsigned long long futex_wake_ops;
	unsigned long long futex_requeue_ops;
	unsigned long long futex_wake_op_ops;
	unsigned long long futex_pi_ops;
	unsigned long long futex_waitv_syscalls;
	unsigned long long futex_other_ops;
	unsigned long long futex_wait_completed_ops;
	unsigned long long futex_wait_completed_total_ns;
	unsigned long long futex_wait_blocked_ops;
	unsigned long long futex_wait_blocked_total_ns;
	unsigned long long futex_wait_blocked_max_ns;
	unsigned long long futex_wait_ret[RVV_FUTEX_RET_BUCKETS];
	unsigned long long futex_wait_ret_total_ns[RVV_FUTEX_RET_BUCKETS];
};

struct rvv_waker_stats {
	unsigned int wakee_tgid;
	unsigned int wakee_tid;
	unsigned int waker_tgid;
	unsigned int waker_tid;
	char wakee_comm[RVV_COMM_LEN];
	char waker_comm[RVV_COMM_LEN];
	unsigned long long wakeups;
	unsigned long long wakee_dirty_wakeups;
};

struct rvv_futex_addr_stats {
	unsigned long long uaddr;
	unsigned long long syscalls;
	unsigned long long wait_ops;
	unsigned long long wake_ops;
	unsigned long long requeue_ops;
	unsigned long long wake_op_ops;
	unsigned long long pi_ops;
	unsigned long long waitv_syscalls;
	unsigned long long other_ops;
	unsigned long long wait_completed_ops;
	unsigned long long wait_completed_total_ns;
	unsigned long long wait_blocked_ops;
	unsigned long long wait_blocked_total_ns;
	unsigned long long wait_blocked_max_ns;
	unsigned long long wait_ret[RVV_FUTEX_RET_BUCKETS];
	unsigned long long wait_ret_total_ns[RVV_FUTEX_RET_BUCKETS];
	unsigned int last_tgid;
	unsigned int last_tid;
	char last_comm[RVV_COMM_LEN];
};

struct rvv_futex_tid_stats {
	unsigned long long uaddr;
	unsigned int tgid;
	unsigned int tid;
	char comm[RVV_COMM_LEN];
	unsigned long long syscalls;
	unsigned long long wait_ops;
	unsigned long long wake_ops;
	unsigned long long requeue_ops;
	unsigned long long wake_op_ops;
	unsigned long long pi_ops;
	unsigned long long waitv_syscalls;
	unsigned long long other_ops;
	unsigned long long wait_completed_ops;
	unsigned long long wait_completed_total_ns;
	unsigned long long wait_blocked_ops;
	unsigned long long wait_blocked_total_ns;
	unsigned long long wait_blocked_max_ns;
	unsigned long long wait_ret[RVV_FUTEX_RET_BUCKETS];
	unsigned long long wait_ret_total_ns[RVV_FUTEX_RET_BUCKETS];
};

struct rvv_futex_stack_stats {
	unsigned long long uaddr;
	int stack_id;
	unsigned int kind;
	unsigned int last_tgid;
	unsigned int last_tid;
	char last_comm[RVV_COMM_LEN];
	unsigned long long syscalls;
	unsigned long long wait_ops;
	unsigned long long wake_ops;
	unsigned long long requeue_ops;
	unsigned long long wake_op_ops;
	unsigned long long pi_ops;
	unsigned long long waitv_syscalls;
	unsigned long long other_ops;
};

struct rvv_cpu_stats {
	unsigned int cpu;
	unsigned long long switch_events;
	unsigned long long switches_in;
	unsigned long long switches_out;
	unsigned long long wakeups;
	unsigned long long v_save_candidates;
	unsigned long long v_restore_candidates;
	unsigned long long switches_out_state[RVV_OUT_STATE_BUCKETS];
	unsigned long long v_save_out_state[RVV_OUT_STATE_BUCKETS];
};

#endif
