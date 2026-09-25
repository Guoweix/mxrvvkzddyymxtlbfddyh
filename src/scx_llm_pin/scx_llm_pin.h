// SPDX-License-Identifier: GPL-2.0
#ifndef __SCX_LLM_PIN_H
#define __SCX_LLM_PIN_H

#include "scx_rvv_lease_control.h"

#define LLM_CPU_MASK 0xffU

#define RVV_SR_VS		0x00000600UL
#define RVV_SR_VS_OFF		0x00000000UL
#define RVV_SR_VS_INITIAL	0x00000200UL
#define RVV_SR_VS_CLEAN		0x00000400UL
#define RVV_SR_VS_DIRTY		0x00000600UL

struct llm_team_config {
	__u32 tgid;
	__u32 threads;
	__u32 threads_batch;
	__u32 cpu_mask;
	__u32 cpu_order[RVV_LEASE_MAX_CPUS];
};

struct llm_team_stats {
	__u64 vector_threads;
	__u64 enqueues;
	__u64 consumes;
	__u64 runs;
};

// 共享配置结构，用户态和 BPF 之间通信
struct llm_pin_config {
	__u32 team_count;
	__u32 reserved;
	struct llm_team_config teams[RVV_LEASE_CONTROL_TEAMS];
	__u32 target_tgid;        // 目标进程 TGID（llama-server）
	__u32 target_tgid2;       // 可选的第二个 llama-server TGID
	__u32 enabled;            // 是否启用绑定策略
	__u32 dirty_threshold;    // 多少次 Dirty 采样后标记为 vector 线程
	__u64 vector_slice_ns;    // vector 线程的时间片
	__u64 vector_local_window_ns; // vector DSQ 放行后的 local 快速窗口
	__u64 user_session_id;    // ABI v4 worker page 会话标识
	__u64 user_policy_epoch;  // ABI v4 worker page 策略纪元
	__u64 phase_hint_timeout_ns; // 拒绝 stale phase hint 的时限
	__u64 prefill_starvation_ns; // prefill-leased vector DSQ 安全等待上限
	__u32 phase_wakeup_preempt_enabled; // preferred wakeup 是否定向抢占
	__u32 reserved_phase;
};

// 统计信息
struct llm_pin_stats {
	struct llm_team_stats teams[RVV_LEASE_CONTROL_TEAMS];
	__u64 llm_scheduled;      // LLM 线程调度次数
	__u64 llm_pinned;         // LLM 线程被固定到指定 CPU 的次数
	__u64 system_scheduled;   // 系统线程调度次数
	__u64 cpu_migrations;     // 跨核迁移次数（应该降为 0）
	__u64 llm_local_dispatches;
	__u64 system_dispatches;
	__u64 rvv_dirty_samples;
	__u64 rvv_vector_threads;
	__u64 rvv_vector_threads_team0;
	__u64 rvv_vector_threads_team1;
	__u64 rvv_vector_dispatches;
	__u64 rvv_nonvector_llm_dispatches;
	__u64 rvv_vector_dsq_dispatches;
	__u64 rvv_vector_dsq_consumes;
	__u64 rvv_vector_dsq_dispatches_team0;
	__u64 rvv_vector_dsq_dispatches_team1;
	__u64 rvv_vector_dsq_consumes_team0;
	__u64 rvv_vector_dsq_consumes_team1;
	__u64 rvv_vector_slice_clamps;
	__u64 rvv_vector_local_dispatches;
	__u64 rvv_vector_window_starts;
	__u64 phase_hint_reads;
	__u64 phase_hint_valid;
	__u64 phase_hint_invalid;
	__u64 phase_hint_idle;
	__u64 phase_hint_prefill;
	__u64 phase_hint_decode;
	__u64 phase_hint_mixed;
	__u64 phase_lease_engagements;
	__u64 phase_lease_preferred_consumes;
	__u64 phase_lease_empty_fallbacks;
	__u64 phase_lease_fallback_consumes;
	__u64 phase_lease_safety_forced;
	__u64 phase_lease_safety_misses;
	__u64 phase_lease_pending_stale;
	__u64 phase_lease_pending_max_ns;
	__u64 phase_lease_pending_ge_4s;
	__u64 phase_lease_running_samples;
	__u64 phase_lease_preferred_runs;
	__u64 phase_lease_other_runs;
	__u64 phase_lease_wakeup_candidates;
	__u64 phase_lease_wakeup_preempts;
};

struct rvv_task_class {
	__u32 dirty_score;
	__u32 is_vector;
	__u32 team_id_plus1;
	__u32 assigned_cpu_plus1;
	__u32 running;
	__u32 reserved32;
};

#endif /* __SCX_LLM_PIN_H */
