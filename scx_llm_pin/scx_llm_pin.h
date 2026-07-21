// SPDX-License-Identifier: GPL-2.0
#ifndef __SCX_LLM_PIN_H
#define __SCX_LLM_PIN_H

#include "scx_rvv_lease_control.h"

#define LLM_CPU_MASK 0xffU

enum llm_sched_strategy {
	LLM_STRATEGY_GLOBAL = 0,     // 最小 sched_ext 全局队列
	LLM_STRATEGY_RVV_DIRTY = 4,  // 根据 RVV Dirty 状态识别向量线程
	LLM_STRATEGY_RVV_DIRTY_LOCAL = 5, // RVV Dirty + nonvector LLM 本地 DSQ
};

enum llm_team_gang_mode {
	LLM_TEAM_GANG_OFF = 0,
	LLM_TEAM_GANG_OBSERVE = 1,
	LLM_TEAM_GANG_ALWAYS = 3,
	LLM_TEAM_GANG_PREFILL_LEASE = 4,
};

enum llm_vector_local_wakeup_mode {
	LLM_VECTOR_LOCAL_WAKEUP_TEAM = 0,
	LLM_VECTOR_LOCAL_WAKEUP_OFF = 1,
	LLM_VECTOR_LOCAL_WAKEUP_EXCLUSIVE = 2,
};

#define RVV_SR_VS		0x00000600UL
#define RVV_SR_VS_OFF		0x00000000UL
#define RVV_SR_VS_INITIAL	0x00000200UL
#define RVV_SR_VS_CLEAN		0x00000400UL
#define RVV_SR_VS_DIRTY		0x00000600UL

// 共享配置结构，用户态和 BPF 之间通信
struct llm_pin_config {
	__u32 target_tgid;        // 目标进程 TGID（llama-server）
	__u32 target_tgid2;       // 可选的第二个 llama-server TGID
	__u32 enabled;            // 是否启用绑定策略
	__u32 strategy;           // enum llm_sched_strategy
	__u32 dirty_threshold;    // 多少次 Dirty 采样后标记为 vector 线程
	__u64 vector_slice_ns;    // vector 线程的时间片
	__u64 vector_local_window_ns; // vector DSQ 放行后的 local 快速窗口
	__u32 user_control_mode; // enum rvv_lease_control_mode
	__u32 user_cpu_map_enabled; // 1=显式 ith->CPU；0=保留 lease 自然唯一分配
	__u32 user_preferred_team_plus1; // 0=禁用；1/2=team 0/1
	__u32 phase_wakeup_preempt_enabled; // preferred wakeup 是否定向抢占
	__u32 user_cpu_map[RVV_LEASE_CONTROL_WORKERS]; // OpenMP ith -> CPU
	__u64 user_preferred_until_ns; // CLOCK_MONOTONIC 绝对过期时间
	__u64 user_session_id;
	__u64 user_policy_epoch;
	__u32 team_gang_mode; // enum llm_team_gang_mode
	__u32 vector_local_wakeup_mode; // enum llm_vector_local_wakeup_mode
	__u64 gang_quantum_ns;
	__u64 phase_hint_timeout_ns;
	__u64 prefill_starvation_ns; // Max runnable wait before one safety slice
};

// 统计信息
struct llm_pin_stats {
	__u64 llm_scheduled;      // LLM 线程调度次数
	__u64 llm_pinned;         // LLM 线程被固定到指定 CPU 的次数
	__u64 system_scheduled;   // 系统线程调度次数
	__u64 cpu_migrations;     // 跨核迁移次数（应该降为 0）
	__u64 llm_global_dispatches;
	__u64 llm_local_dispatches;
	__u64 system_dispatches;
	__u64 rvv_dirty_samples;
	__u64 rvv_vector_threads;
	__u64 rvv_vector_threads_team0;
	__u64 rvv_vector_threads_team1;
	__u64 rvv_vector_dispatches;
	__u64 rvv_nonvector_llm_dispatches;
	__u64 protected_kernel_dispatches;
	__u64 protected_kernel_preempts;
	__u64 rvv_vector_dsq_dispatches;
	__u64 rvv_vector_dsq_consumes;
	__u64 rvv_vector_dsq_dispatches_team0;
	__u64 rvv_vector_dsq_dispatches_team1;
	__u64 rvv_vector_dsq_consumes_team0;
	__u64 rvv_vector_dsq_consumes_team1;
	__u64 rvv_vector_local_dispatches;
	__u64 rvv_vector_window_starts;
	__u64 rvv_vector_slice_clamps;
	__u64 rvv_vector_identify_barrier_dispatches;
	__u64 user_hint_classification_reads;
	__u64 user_hint_valid_workers;
	__u64 user_hint_observed_workers;
	__u64 user_hint_active_workers;
	__u64 user_hint_active_assignments;
	__u64 user_hint_invalid_workers;
	__u64 user_hint_fallbacks;
	__u64 user_preference_dsq_consumes;
	__u64 user_preference_empty_fallbacks;
	__u64 user_hint_primary_workers;
	__u64 user_hint_member_misses;
	__u64 user_hint_deferred_tasks;
	__u64 user_hint_excluded_tasks;
	__u64 user_hint_demotions;
	__u64 phase_hint_reads;
	__u64 phase_hint_valid;
	__u64 phase_hint_invalid;
	__u64 phase_hint_idle;
	__u64 phase_hint_prefill;
	__u64 phase_hint_decode;
	__u64 phase_hint_mixed;
	__u64 gang_engagements;
	__u64 gang_quanta;
	__u64 gang_team_switches;
	__u64 gang_cpu_kicks;
	__u64 gang_vector_enqueues;
	__u64 gang_selected_consumes;
	__u64 gang_empty_fallbacks;
	__u64 gang_fallback_consumes;
	__u64 gang_running_samples;
	__u64 gang_active_team_runs;
	__u64 gang_wrong_team_runs;
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
	__u64 vector_exclusive_lease_handoffs;
	__u64 vector_exclusive_lease_denials;
};

struct rvv_task_class {
	__u32 dirty_score;
	__u32 is_vector;
	__u32 team_id_plus1;
	__u32 assigned_cpu_plus1;
	__u32 user_ith_plus1;
	__u32 user_cpu_forced;
	__u32 user_hint_primary;
	__u32 running;
	__u32 user_hint_excluded;
	__u32 reserved32;
	__u64 user_session_id;
	__u64 user_hint_retry_after_ns;
	__u64 user_hint_first_seen_ns;
};

#endif /* __SCX_LLM_PIN_H */
