// SPDX-License-Identifier: GPL-2.0
#ifndef __SCX_LLM_PIN_H
#define __SCX_LLM_PIN_H

enum llm_sched_strategy {
	LLM_STRATEGY_GLOBAL = 0,     // 最小 sched_ext 全局队列
	LLM_STRATEGY_PIN = 1,        // CPU mask 亲和性绑定
	LLM_STRATEGY_LONG_SLICE = 2, // LLM 使用更长时间片
	LLM_STRATEGY_LOCAL_DSQ = 3,  // LLM 优先进入本地 DSQ
	LLM_STRATEGY_RVV_DIRTY = 4,  // 根据 RVV Dirty 状态识别向量线程
	LLM_STRATEGY_RVV_DIRTY_LOCAL = 5, // RVV Dirty + nonvector LLM 本地 DSQ
};

#define RVV_SR_VS		0x00000600UL
#define RVV_SR_VS_OFF		0x00000000UL
#define RVV_SR_VS_INITIAL	0x00000200UL
#define RVV_SR_VS_CLEAN		0x00000400UL
#define RVV_SR_VS_DIRTY		0x00000600UL

// 共享配置结构，用户态和 BPF 之间通信
struct llm_pin_config {
	__u32 target_tgid;        // 目标进程 TGID（llama-server）
	__u32 llm_cpu_mask;       // LLM 专用 CPU 掩码（bit 0-7 对应 CPU 0-7）
	__u32 system_cpu_mask;    // 系统进程 CPU 掩码
	__u32 enabled;            // 是否启用绑定策略
	__u32 strategy;           // enum llm_sched_strategy
	__u32 switch_all;         // 1=接管所有任务，0=仅接管显式 SCHED_EXT 任务
	__u64 llm_slice_ns;       // LLM 时间片，0 表示默认
	__u64 system_slice_ns;    // 系统任务时间片，0 表示默认
	__u32 vector_cpu_mask;    // RVV Dirty vector 线程的激进计算核心
	__u32 ordinary_cpu_mask;  // RVV Dirty fallback/普通核心
	__u32 dirty_threshold;    // 多少次 Dirty 采样后标记为 vector 线程
	__u32 fallback_every;     // 每 N 次 vector 选择走普通核心，0=禁用
	__u64 vector_slice_ns;    // vector 线程在激进核心上的时间片
	__u64 ordinary_vector_slice_ns; // vector 线程在普通核心上的时间片
};

// 统计信息
struct llm_pin_stats {
	__u64 llm_scheduled;      // LLM 线程调度次数
	__u64 llm_pinned;         // LLM 线程被固定到指定 CPU 的次数
	__u64 system_scheduled;   // 系统线程调度次数
	__u64 cpu_migrations;     // 跨核迁移次数（应该降为 0）
	__u64 llm_global_dispatches;
	__u64 llm_local_dispatches;
	__u64 llm_long_slices;
	__u64 system_dispatches;
	__u64 rvv_dirty_samples;
	__u64 rvv_vector_threads;
	__u64 rvv_vector_dispatches;
	__u64 rvv_aggressive_dispatches;
	__u64 rvv_fallback_dispatches;
	__u64 rvv_nonvector_llm_dispatches;
	__u64 protected_kernel_dispatches;
	__u64 protected_kernel_preempts;
};

struct rvv_task_class {
	__u32 dirty_score;
	__u32 is_vector;
};

#endif /* __SCX_LLM_PIN_H */
