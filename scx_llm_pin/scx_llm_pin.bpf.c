// SPDX-License-Identifier: GPL-2.0
/*
 * scx_llm_pin - CPU affinity pinning scheduler for LLM inference workloads
 *
 * 将 llama-server 工作线程固定到指定 CPU 核心，避免跨核迁移和频繁上下文切换。
 */
#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
#include <bpf/bpf_core_read.h>
#include "scx_llm_pin.h"

char LICENSE[] SEC("license") = "GPL";

#ifndef BPF_NOEXIST
#define BPF_NOEXIST 1
#endif
#ifndef BPF_ANY
#define BPF_ANY 0
#endif

// 导入 sched_ext kfuncs
void scx_bpf_dispatch(struct task_struct *p, u64 dsq_id, u64 slice,
		      u64 enq_flags) __ksym;
bool scx_bpf_consume(u64 dsq_id) __ksym;
void scx_bpf_switch_all(void) __ksym;
s32 scx_bpf_pick_idle_cpu(const cpumask_t *cpus_allowed, u64 flags) __ksym;
void scx_bpf_kick_cpu(s32 cpu, u64 flags) __ksym;

#ifndef PF_KTHREAD
#define PF_KTHREAD 0x00200000
#endif

// 配置 map（单元素，用户态写入）
struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, 1);
	__type(key, u32);
	__type(value, struct llm_pin_config);
} config_map SEC(".maps");

// 统计信息 map
struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, 1);
	__type(key, u32);
	__type(value, struct llm_pin_stats);
} stats_map SEC(".maps");

// Per-CPU 统计信息
struct {
	__uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
	__uint(max_entries, 1);
	__type(key, u32);
	__type(value, __u64);
} percpu_stats SEC(".maps");

// RVV Dirty 线程分类状态，key 为 TID
struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__uint(max_entries, 4096);
	__type(key, __u32);
	__type(value, struct rvv_task_class);
} rvv_task_classes SEC(".maps");

// select_cpu 中选出的 CPU，enqueue 中用于决定激进/普通时间片
struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__uint(max_entries, 4096);
	__type(key, __u32);
	__type(value, __s32);
} selected_cpu_map SEC(".maps");

// Per-CPU 调度计数器（用于轮询选择）
struct {
	__uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
	__uint(max_entries, 1);
	__type(key, u32);
	__type(value, __u64);
} cpu_select_counter SEC(".maps");

// 辅助函数：检查任务是否属于目标 LLM 进程
static inline bool is_llm_task(struct task_struct *p, __u32 target_tgid)
{
	if (!target_tgid)
		return false;

	// 检查 TGID（进程组）
	pid_t tgid;
	bpf_probe_read_kernel(&tgid, sizeof(tgid), &p->tgid);

	return tgid == target_tgid;
}

static __always_inline bool is_rvv_dirty_strategy(__u32 strategy)
{
	return strategy == LLM_STRATEGY_RVV_DIRTY ||
	       strategy == LLM_STRATEGY_RVV_DIRTY_LOCAL;
}

static __always_inline bool is_rvv_dirty_local_strategy(__u32 strategy)
{
	return strategy == LLM_STRATEGY_RVV_DIRTY_LOCAL;
}

static __always_inline __u32 task_pid(struct task_struct *p)
{
	pid_t pid;

	bpf_probe_read_kernel(&pid, sizeof(pid), &p->pid);
	return (__u32)pid;
}

static __always_inline bool is_protected_kernel_task(struct task_struct *p)
{
	unsigned int flags;
	int nr_cpus_allowed;

	flags = BPF_CORE_READ(p, flags);
	nr_cpus_allowed = BPF_CORE_READ(p, nr_cpus_allowed);

	return (flags & PF_KTHREAD) && nr_cpus_allowed == 1;
}

static __always_inline bool is_latency_kernel_task(struct task_struct *p)
{
	char comm[16];

	if (bpf_probe_read_kernel_str(comm, sizeof(comm), p->comm) <= 0)
		return false;

	if (comm[0] == 'k' && comm[1] == 'w' && comm[2] == 'o' &&
	    comm[3] == 'r' && comm[4] == 'k' && comm[5] == 'e' &&
	    comm[6] == 'r' && comm[7] == '/')
		return true;

	if (comm[0] == 'k' && comm[1] == 's' && comm[2] == 'o' &&
	    comm[3] == 'f' && comm[4] == 't' && comm[5] == 'i' &&
	    comm[6] == 'r' && comm[7] == 'q' && comm[8] == 'd' &&
	    comm[9] == '/')
		return true;

	return false;
}

static __always_inline bool is_protected_fastlane_task(struct task_struct *p)
{
	return is_protected_kernel_task(p) && is_latency_kernel_task(p);
}

static __always_inline unsigned long read_vs_state(struct task_struct *task,
						   bool *ok)
{
	struct pt_regs *regs;
	unsigned long status;

	*ok = false;
	regs = (struct pt_regs *)bpf_task_pt_regs(task);
	if (!regs)
		return 0;

	status = BPF_CORE_READ(regs, status);
	*ok = true;
	return status & RVV_SR_VS;
}

static __always_inline bool cpu_in_mask(s32 cpu, __u32 cpu_mask)
{
	return cpu >= 0 && cpu < 32 && (cpu_mask & (1U << cpu));
}

static __always_inline struct rvv_task_class *get_rvv_task_class(__u32 tid)
{
	struct rvv_task_class zero = {};
	struct rvv_task_class *state;

	state = bpf_map_lookup_elem(&rvv_task_classes, &tid);
	if (state)
		return state;

	bpf_map_update_elem(&rvv_task_classes, &tid, &zero, BPF_NOEXIST);
	return bpf_map_lookup_elem(&rvv_task_classes, &tid);
}

static __always_inline bool classify_rvv_vector_task(struct task_struct *p,
						     const struct llm_pin_config *cfg,
						     struct llm_pin_stats *stats)
{
	struct rvv_task_class *state;
	__u32 tid = task_pid(p);
	unsigned long vs;
	bool ok = false;
	__u32 threshold;

	if (!is_rvv_dirty_strategy(cfg->strategy))
		return false;

	state = get_rvv_task_class(tid);
	if (!state)
		return false;

	vs = read_vs_state(p, &ok);
	if (ok && vs == RVV_SR_VS_DIRTY) {
		if (state->dirty_score < 0xffffffffU)
			state->dirty_score++;
		if (stats)
			__sync_fetch_and_add(&stats->rvv_dirty_samples, 1);
	}

	threshold = cfg->dirty_threshold ? cfg->dirty_threshold : 2;
	if (!state->is_vector && state->dirty_score >= threshold) {
		state->is_vector = 1;
		if (stats)
			__sync_fetch_and_add(&stats->rvv_vector_threads, 1);
	}

	return state->is_vector;
}

static __always_inline void remember_selected_cpu(struct task_struct *p, s32 cpu)
{
	__u32 tid = task_pid(p);

	bpf_map_update_elem(&selected_cpu_map, &tid, &cpu, BPF_ANY);
}

static __always_inline s32 remembered_selected_cpu(struct task_struct *p)
{
	__u32 tid = task_pid(p);
	__s32 *cpu;

	cpu = bpf_map_lookup_elem(&selected_cpu_map, &tid);
	if (!cpu)
		return -1;
	return *cpu;
}

static __always_inline s32 protected_kernel_cpu(struct task_struct *p, s32 fallback_cpu)
{
	s32 cpu = remembered_selected_cpu(p);

	if (cpu >= 0 && cpu < 32)
		return cpu;

	if (fallback_cpu >= 0 && fallback_cpu < 32)
		return fallback_cpu;

	return (s32)bpf_get_smp_processor_id();
}

// 辅助函数：从 CPU mask 中选择 CPU（简单轮询，BPF验证器友好）
static inline s32 select_cpu_from_mask(__u32 cpu_mask, s32 prev_cpu)
{
	if (!cpu_mask)
		return prev_cpu;

	// 优先保持在原 CPU（如果在 mask 内）
	if (prev_cpu >= 0 && prev_cpu < 32 && (cpu_mask & (1U << prev_cpu)))
		return prev_cpu;

	// BPF验证器要求固定大小的循环和边界
	// 使用简单的线性搜索，从 prev_cpu+1 开始轮询
	u32 start_cpu = (prev_cpu >= 0 && prev_cpu < 8) ? (u32)(prev_cpu + 1) : 0;

	// 从 start_cpu 开始查找第一个可用CPU
	#pragma unroll
	for (u32 offset = 0; offset < 8; offset++) {
		u32 cpu = (start_cpu + offset) & 7;  // 模8，保持在0-7范围
		if (cpu_mask & (1U << cpu))
			return (s32)cpu;
	}

	// 如果没有找到（理论上不应该发生），返回原CPU
	return prev_cpu;
}

static __always_inline s32 select_rvv_vector_cpu(struct task_struct *p,
						  const struct llm_pin_config *cfg,
						  struct llm_pin_stats *stats,
						  s32 prev_cpu)
{
	__u32 aggressive_mask = cfg->vector_cpu_mask ? cfg->vector_cpu_mask : cfg->llm_cpu_mask;
	__u32 ordinary_mask = cfg->ordinary_cpu_mask;
	__u32 selected_mask = aggressive_mask;
	__u32 key = 0;
	__u64 *counter;
	s32 cpu;

	if (cpu_in_mask(prev_cpu, aggressive_mask) || cpu_in_mask(prev_cpu, ordinary_mask)) {
		remember_selected_cpu(p, prev_cpu);
		return prev_cpu;
	}

	if (cfg->fallback_every && ordinary_mask) {
		counter = bpf_map_lookup_elem(&cpu_select_counter, &key);
		if (counter) {
			__u64 next = *counter + 1;
			*counter = next;
			if ((next % cfg->fallback_every) == 0)
				selected_mask = ordinary_mask;
		}
	}

	cpu = select_cpu_from_mask(selected_mask, prev_cpu);
	remember_selected_cpu(p, cpu);

	if (stats && cpu != prev_cpu)
		__sync_fetch_and_add(&stats->cpu_migrations, 1);

	return cpu;
}

SEC("struct_ops/scx_llm_pin_select_cpu")
s32 BPF_PROG(scx_llm_pin_select_cpu, struct task_struct *p, s32 prev_cpu,
	     u64 wake_flags)
{
	struct llm_pin_config *cfg;
	struct llm_pin_stats *stats;
	u32 key = 0;

	cfg = bpf_map_lookup_elem(&config_map, &key);
	if (!cfg || !cfg->enabled)
		return prev_cpu;  // 默认保持在原CPU

	if (cfg->strategy == LLM_STRATEGY_GLOBAL)
		return prev_cpu;

	stats = bpf_map_lookup_elem(&stats_map, &key);

	// 根据任务类型选择CPU
	if (!is_llm_task(p, cfg->target_tgid) && is_protected_fastlane_task(p)) {
		s32 cpu = prev_cpu;

		remember_selected_cpu(p, cpu);
		if (stats) {
			__sync_fetch_and_add(&stats->system_scheduled, 1);
		}
		return cpu;
	}

	if (is_llm_task(p, cfg->target_tgid)) {
		s32 cpu;

		if (is_rvv_dirty_strategy(cfg->strategy) &&
		    classify_rvv_vector_task(p, cfg, stats)) {
			cpu = select_rvv_vector_cpu(p, cfg, stats, prev_cpu);
			if (stats) {
				__sync_fetch_and_add(&stats->llm_scheduled, 1);
				__sync_fetch_and_add(&stats->llm_pinned, 1);
			}
			return cpu;
		}

		// LLM任务 -> 固定到 llm_cpu_mask
		cpu = select_cpu_from_mask(cfg->llm_cpu_mask, prev_cpu);
		remember_selected_cpu(p, cpu);
		if (stats) {
			__sync_fetch_and_add(&stats->llm_scheduled, 1);
			__sync_fetch_and_add(&stats->llm_pinned, 1);
			if (cpu != prev_cpu)
				__sync_fetch_and_add(&stats->cpu_migrations, 1);
		}
		return cpu;
	} else {
		s32 cpu;

		// 系统任务 -> 固定到 system_cpu_mask
		cpu = select_cpu_from_mask(cfg->system_cpu_mask, prev_cpu);
		if (stats) {
			__sync_fetch_and_add(&stats->system_scheduled, 1);
			if (cpu != prev_cpu)
				__sync_fetch_and_add(&stats->cpu_migrations, 1);
		}
		return cpu;
	}
}

SEC("struct_ops/scx_llm_pin_enqueue")
void BPF_PROG(scx_llm_pin_enqueue, struct task_struct *p, u64 enq_flags)
{
	struct llm_pin_config *cfg;
	struct llm_pin_stats *stats;
	u64 slice = SCX_SLICE_DFL;
	u32 key = 0;

	cfg = bpf_map_lookup_elem(&config_map, &key);
	stats = bpf_map_lookup_elem(&stats_map, &key);

	if (!cfg || !cfg->enabled || cfg->strategy == LLM_STRATEGY_GLOBAL) {
		scx_bpf_dispatch(p, SCX_DSQ_GLOBAL, SCX_SLICE_DFL, enq_flags);
		return;
	}

	if (!is_llm_task(p, cfg->target_tgid) &&
	    is_protected_fastlane_task(p) &&
	    (enq_flags & SCX_ENQ_WAKEUP)) {
		s32 cpu = protected_kernel_cpu(p, bpf_get_smp_processor_id());
		u64 protected_flags = enq_flags | SCX_ENQ_HEAD | SCX_ENQ_PREEMPT;

		if (stats) {
			__sync_fetch_and_add(&stats->system_dispatches, 1);
			__sync_fetch_and_add(&stats->protected_kernel_dispatches, 1);
			__sync_fetch_and_add(&stats->protected_kernel_preempts, 1);
		}

		scx_bpf_dispatch(p, SCX_DSQ_LOCAL, SCX_SLICE_DFL, protected_flags);
		scx_bpf_kick_cpu(cpu, SCX_KICK_PREEMPT);
		return;
	}

	if (is_llm_task(p, cfg->target_tgid)) {
		if (is_rvv_dirty_strategy(cfg->strategy) &&
		    classify_rvv_vector_task(p, cfg, stats)) {
			s32 selected_cpu = remembered_selected_cpu(p);

			if (!cpu_in_mask(selected_cpu, cfg->vector_cpu_mask) &&
			    cpu_in_mask(selected_cpu, cfg->ordinary_cpu_mask)) {
				if (cfg->ordinary_vector_slice_ns)
					slice = cfg->ordinary_vector_slice_ns;
				else if (cfg->llm_slice_ns)
					slice = cfg->llm_slice_ns;
				if (stats)
					__sync_fetch_and_add(&stats->rvv_fallback_dispatches, 1);
			} else {
				if (cfg->vector_slice_ns)
					slice = cfg->vector_slice_ns;
				else if (cfg->llm_slice_ns)
					slice = cfg->llm_slice_ns;
				if (stats)
					__sync_fetch_and_add(&stats->rvv_aggressive_dispatches, 1);
			}

			if (stats) {
				__sync_fetch_and_add(&stats->rvv_vector_dispatches, 1);
				__sync_fetch_and_add(&stats->llm_local_dispatches, 1);
			}

			scx_bpf_dispatch(p, SCX_DSQ_LOCAL, slice, enq_flags);
			return;
		}

		if (cfg->llm_slice_ns)
			slice = cfg->llm_slice_ns;

		if (is_rvv_dirty_strategy(cfg->strategy) && stats)
			__sync_fetch_and_add(&stats->rvv_nonvector_llm_dispatches, 1);

		if (cfg->strategy == LLM_STRATEGY_LONG_SLICE && stats)
			__sync_fetch_and_add(&stats->llm_long_slices, 1);

		if (cfg->strategy == LLM_STRATEGY_LOCAL_DSQ ||
		    is_rvv_dirty_local_strategy(cfg->strategy)) {
			if (stats)
				__sync_fetch_and_add(&stats->llm_local_dispatches, 1);
			scx_bpf_dispatch(p, SCX_DSQ_LOCAL, slice, enq_flags);
		} else {
			if (stats)
				__sync_fetch_and_add(&stats->llm_global_dispatches, 1);
			scx_bpf_dispatch(p, SCX_DSQ_GLOBAL, slice, enq_flags);
		}
	} else {
		if (cfg->system_slice_ns)
			slice = cfg->system_slice_ns;
		if (stats)
			__sync_fetch_and_add(&stats->system_dispatches, 1);
		scx_bpf_dispatch(p, SCX_DSQ_GLOBAL, slice, enq_flags);
	}
}

SEC("struct_ops/scx_llm_pin_dispatch")
void BPF_PROG(scx_llm_pin_dispatch, s32 cpu, struct task_struct *prev)
{
	// 循环消费全局队列中的所有任务，直到队列为空
	// 这可以减少调度延迟
	if (!scx_bpf_consume(SCX_DSQ_GLOBAL))
		return;
}

SEC("struct_ops/scx_llm_pin_init")
s32 BPF_PROG(scx_llm_pin_init)
{
	struct llm_pin_config *cfg;
	u32 key = 0;

	cfg = bpf_map_lookup_elem(&config_map, &key);
	if (!cfg || cfg->switch_all)
		scx_bpf_switch_all();

	return 0;
}

SEC("struct_ops/scx_llm_pin_exit")
void BPF_PROG(scx_llm_pin_exit, struct scx_exit_info *ei)
{
	// 清理工作（如果需要）
}

SEC(".struct_ops")
struct sched_ext_ops scx_llm_pin_ops = {
	.select_cpu		= (void *)scx_llm_pin_select_cpu,
	.enqueue		= (void *)scx_llm_pin_enqueue,
	.dispatch		= (void *)scx_llm_pin_dispatch,
	.init			= (void *)scx_llm_pin_init,
	.exit			= (void *)scx_llm_pin_exit,
	.dispatch_max_batch	= 1,
	.timeout_ms		= 5000,
	.name			= "scx_llm_pin",
};
