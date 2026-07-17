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
s32 scx_bpf_create_dsq(u64 dsq_id, s32 node) __ksym;

#define VECTOR_DSQ_BASE 0x100000ULL

// 配置 map（单元素，用户态写入）
struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, 1);
	__type(key, u32);
	__type(value, struct llm_pin_config);
} config_map SEC(".maps");

// Optional userspace worker registration. The hot path never reads this map
// after a task has been classified as an RVV worker.
struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, 1);
	__uint(map_flags, BPF_F_MMAPABLE);
	__type(key, u32);
	__type(value, struct rvv_lease_worker_page);
} user_worker_map SEC(".maps");

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

// 每个 CPU 的 vector 线程所有者 TID，0 表示未分配
struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, 8);
	__type(key, __u32);
	__type(value, __u32);
} vector_cpu_owners SEC(".maps");

// 每个 CPU 当前 vector-local 窗口的结束时间戳（ns）
struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, 8);
	__type(key, __u32);
	__type(value, __u64);
} vector_local_until_ns SEC(".maps");

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

struct lease_worker_snapshot {
	__u32 tid;
	__u32 tgid;
	__u32 ith;
	__u32 n_threads;
	__u64 session_id;
};

static __always_inline bool read_lease_worker_slot(
	const struct rvv_lease_worker_page *page, __u32 index,
	struct lease_worker_snapshot *out)
{
	const struct rvv_lease_worker_slot *slot;
	__u32 sequence;

	if (!page || index >= RVV_LEASE_CONTROL_WORKERS)
		return false;
	slot = &page->workers[index];
	sequence = slot->sequence;
	if (!sequence || (sequence & 1U))
		return false;
	out->tid = slot->tid;
	out->tgid = slot->tgid;
	out->ith = slot->ith;
	out->n_threads = slot->n_threads;
	out->session_id = slot->session_id;
	return sequence == slot->sequence;
}

static __always_inline void apply_user_worker_hint(
	const struct llm_pin_config *cfg, struct llm_pin_stats *stats,
	struct rvv_task_class *state, __u32 tid)
{
	struct rvv_lease_worker_page *page;
	struct lease_worker_snapshot slot = {};
	__u32 allowed_mask;
	__u32 key = 0;
	__u32 cpu;

	if (!cfg || !state || cfg->user_control_mode == RVV_LEASE_CONTROL_OFF)
		return;
	if (stats)
		__sync_fetch_and_add(&stats->user_hint_classification_reads, 1);
	page = bpf_map_lookup_elem(&user_worker_map, &key);
	if (!page || page->header.magic != RVV_LEASE_CONTROL_MAGIC ||
	    page->header.abi_version != RVV_LEASE_CONTROL_ABI_VERSION ||
	    !page->header.active || page->header.target_tgid != cfg->target_tgid ||
	    page->header.max_workers != RVV_LEASE_CONTROL_WORKERS ||
	    !cfg->user_session_id ||
	    page->header.session_id != cfg->user_session_id) {
		if (stats)
			__sync_fetch_and_add(&stats->user_hint_fallbacks, 1);
		return;
	}

#define TRY_WORKER_SLOT(index) do {                                           \
		if (!read_lease_worker_slot(page, (index), &slot))               \
			break;                                                    \
		if (slot.tid != tid)                                             \
			break;                                                    \
		if (slot.tgid != cfg->target_tgid || slot.ith != (index) ||       \
		    slot.n_threads != RVV_LEASE_CONTROL_WORKERS ||                \
		    slot.session_id != cfg->user_session_id) {                    \
			if (stats)                                                  \
				__sync_fetch_and_add(&stats->user_hint_invalid_workers, 1); \
			return;                                                       \
		}                                                                  \
		state->user_ith_plus1 = (index) + 1;                               \
		state->user_session_id = slot.session_id;                          \
		if (stats)                                                         \
			__sync_fetch_and_add(&stats->user_hint_valid_workers, 1);     \
		if (cfg->user_control_mode == RVV_LEASE_CONTROL_OBSERVE) {         \
			if (stats)                                                     \
				__sync_fetch_and_add(&stats->user_hint_observed_workers, 1); \
			return;                                                       \
		}                                                                  \
		if (stats)                                                         \
			__sync_fetch_and_add(&stats->user_hint_active_workers, 1);     \
		if (!cfg->user_cpu_map_enabled)                                    \
			return;                                                       \
		cpu = cfg->user_cpu_map[(index)];                                  \
		allowed_mask = cfg->vector_cpu_mask | cfg->ordinary_cpu_mask;      \
		if (!allowed_mask)                                                 \
			allowed_mask = cfg->llm_cpu_mask;                            \
		if (!cpu_in_mask((s32)cpu, allowed_mask)) {                        \
			if (stats)                                                     \
				__sync_fetch_and_add(&stats->user_hint_invalid_workers, 1); \
			return;                                                       \
		}                                                                  \
		state->assigned_cpu_plus1 = cpu + 1;                               \
		state->user_cpu_forced = 1;                                       \
		if (stats)                                                         \
			__sync_fetch_and_add(&stats->user_hint_active_assignments, 1); \
		return;                                                            \
	} while (0)

	TRY_WORKER_SLOT(0);
	TRY_WORKER_SLOT(1);
	TRY_WORKER_SLOT(2);
	TRY_WORKER_SLOT(3);
	TRY_WORKER_SLOT(4);
	TRY_WORKER_SLOT(5);
	TRY_WORKER_SLOT(6);
	TRY_WORKER_SLOT(7);
#undef TRY_WORKER_SLOT

	if (stats)
		__sync_fetch_and_add(&stats->user_hint_fallbacks, 1);
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
		apply_user_worker_hint(cfg, stats, state, tid);
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

static __always_inline u64 vector_dsq_id(s32 cpu)
{
	return VECTOR_DSQ_BASE + (u64)(u32)cpu;
}

static __always_inline __u64 clamp_slice(__u64 slice, __u64 limit,
					 struct llm_pin_stats *stats)
{
	if (limit && slice > limit) {
		if (stats)
			__sync_fetch_and_add(&stats->rvv_vector_slice_clamps, 1);
		return limit;
	}
	return slice;
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

static __always_inline bool claim_vector_cpu(__u32 tid, s32 cpu,
					      __u32 allowed_mask)
{
	__u32 cpu_key;
	__u32 *owner;

	if (!cpu_in_mask(cpu, allowed_mask))
		return false;

	cpu_key = (__u32)cpu;
	owner = bpf_map_lookup_elem(&vector_cpu_owners, &cpu_key);
	if (!owner)
		return false;
	if (*owner == tid)
		return true;
	if (*owner == 0 && __sync_val_compare_and_swap(owner, 0, tid) == 0)
		return true;
	return false;
}

static __always_inline void release_vector_cpu(__u32 tid, s32 cpu)
{
	__u32 cpu_key;
	__u32 *owner;

	if (cpu < 0 || cpu >= 8)
		return;
	cpu_key = (__u32)cpu;
	owner = bpf_map_lookup_elem(&vector_cpu_owners, &cpu_key);
	if (owner)
		__sync_val_compare_and_swap(owner, tid, 0);
}

static __always_inline s32 select_unique_vector_cpu(struct task_struct *p,
						      const struct llm_pin_config *cfg,
						      struct llm_pin_stats *stats,
						      s32 prev_cpu)
{
	struct rvv_task_class *state;
	__u32 tid = task_pid(p);
	__u32 allowed_mask = cfg->vector_cpu_mask | cfg->ordinary_cpu_mask;
	s32 assigned_cpu;

	if (!allowed_mask)
		allowed_mask = cfg->llm_cpu_mask;

	state = get_rvv_task_class(tid);
	if (!state)
		return select_cpu_from_mask(allowed_mask, prev_cpu);

	if (cfg->user_control_mode == RVV_LEASE_CONTROL_ACTIVE &&
	    cfg->user_cpu_map_enabled && state->user_ith_plus1 &&
	    state->user_session_id == cfg->user_session_id) {
		__u32 ith = state->user_ith_plus1 - 1;

		if (ith < RVV_LEASE_CONTROL_WORKERS) {
			s32 user_cpu = (s32)cfg->user_cpu_map[ith];

			if (cpu_in_mask(user_cpu, allowed_mask)) {
				if (!state->user_cpu_forced ||
				    state->assigned_cpu_plus1 != (__u32)user_cpu + 1) {
					if (!state->user_cpu_forced &&
					    state->assigned_cpu_plus1)
						release_vector_cpu(
							tid,
							(s32)state->assigned_cpu_plus1 - 1);
					if (stats)
						__sync_fetch_and_add(
							&stats->user_hint_active_assignments,
							1);
				}
				state->assigned_cpu_plus1 = (__u32)user_cpu + 1;
				state->user_cpu_forced = 1;
				return user_cpu;
			}
		}
	}

	if (state->user_cpu_forced) {
		state->assigned_cpu_plus1 = 0;
		state->user_cpu_forced = 0;
	}

	if (state->assigned_cpu_plus1) {
		assigned_cpu = (s32)state->assigned_cpu_plus1 - 1;
		if (claim_vector_cpu(tid, assigned_cpu, allowed_mask))
			return assigned_cpu;
		state->assigned_cpu_plus1 = 0;
	}

	if (claim_vector_cpu(tid, prev_cpu, allowed_mask)) {
		state->assigned_cpu_plus1 = (__u32)prev_cpu + 1;
		return prev_cpu;
	}

	#pragma unroll
	for (__u32 offset = 0; offset < 8; offset++) {
		s32 cpu = (s32)((tid + offset) & 7);

		if (claim_vector_cpu(tid, cpu, allowed_mask)) {
			state->assigned_cpu_plus1 = (__u32)cpu + 1;
			return cpu;
		}
	}

	return select_cpu_from_mask(allowed_mask, prev_cpu);
}

static __always_inline __u32 vector_unique_target(const struct llm_pin_config *cfg)
{
	__u32 allowed_mask = (cfg->vector_cpu_mask | cfg->ordinary_cpu_mask) & 0xff;
	__u32 count = 0;

	if (!allowed_mask)
		allowed_mask = cfg->llm_cpu_mask & 0xff;

	#pragma unroll
	for (__u32 cpu = 0; cpu < 8; cpu++) {
		if (allowed_mask & (1U << cpu))
			count++;
	}
	return count;
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

	if (cfg->vector_local_window_ns) {
		cpu = select_unique_vector_cpu(p, cfg, stats, prev_cpu);
		remember_selected_cpu(p, cpu);
		if (stats && cpu != prev_cpu)
			__sync_fetch_and_add(&stats->cpu_migrations, 1);
		return cpu;
	}

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

	if (is_llm_task(p, cfg->target_tgid)) {
		if (is_rvv_dirty_strategy(cfg->strategy) &&
		    classify_rvv_vector_task(p, cfg, stats)) {
			s32 selected_cpu = remembered_selected_cpu(p);
			u64 dsq_id;
			__u32 cpu_key;
			__u64 *local_until;
			__u64 now;

			if (cfg->vector_local_window_ns)
				selected_cpu = select_unique_vector_cpu(p, cfg, stats,
								 selected_cpu);
			remember_selected_cpu(p, selected_cpu);

			if (selected_cpu < 0 || selected_cpu >= 8)
				selected_cpu = (s32)bpf_get_smp_processor_id();
			if (selected_cpu < 0 || selected_cpu >= 8)
				selected_cpu = 0;

			if (cfg->vector_local_window_ns && stats &&
			    stats->rvv_vector_threads < vector_unique_target(cfg)) {
				cpu_key = (__u32)selected_cpu;
				local_until = bpf_map_lookup_elem(
					&vector_local_until_ns, &cpu_key);
				if (local_until)
					*local_until = bpf_ktime_get_ns() +
						       cfg->vector_local_window_ns;
				__sync_fetch_and_add(&stats->rvv_vector_identify_barrier_dispatches,
						     1);
				scx_bpf_dispatch(p, SCX_DSQ_LOCAL, SCX_SLICE_DFL,
						 enq_flags);
				return;
			}

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
			}

			if (cfg->vector_local_window_ns) {
				cpu_key = (__u32)selected_cpu;
				local_until = bpf_map_lookup_elem(&vector_local_until_ns,
								  &cpu_key);
				now = bpf_ktime_get_ns();
				if (local_until && *local_until > now) {
					slice = clamp_slice(slice, *local_until - now,
							    stats);
					if (stats)
						__sync_fetch_and_add(&stats->rvv_vector_local_dispatches, 1);
					scx_bpf_dispatch(p, SCX_DSQ_LOCAL, slice, enq_flags);
					return;
				}

				slice = clamp_slice(slice, cfg->vector_local_window_ns,
						    stats);
			}

			if (stats)
				__sync_fetch_and_add(&stats->rvv_vector_dsq_dispatches, 1);
			dsq_id = vector_dsq_id(selected_cpu);
			scx_bpf_dispatch(p, dsq_id, slice, enq_flags);
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
	struct llm_pin_config *cfg;
	struct llm_pin_stats *stats;
	u32 key = 0;

	if (scx_bpf_consume(SCX_DSQ_GLOBAL))
		return;

	if (cpu >= 0 && cpu < 8 && scx_bpf_consume(vector_dsq_id(cpu))) {
		__u32 cpu_key = (__u32)cpu;
		__u64 *local_until;

		cfg = bpf_map_lookup_elem(&config_map, &key);
		stats = bpf_map_lookup_elem(&stats_map, &key);
		if (cfg && cfg->vector_local_window_ns) {
			__u64 now = bpf_ktime_get_ns();

			local_until = bpf_map_lookup_elem(&vector_local_until_ns,
							  &cpu_key);
			if (local_until) {
				*local_until = now + cfg->vector_local_window_ns;
				if (stats)
					__sync_fetch_and_add(&stats->rvv_vector_window_starts, 1);
			}
		}
		if (stats)
			__sync_fetch_and_add(&stats->rvv_vector_dsq_consumes, 1);
		return;
	}
}

SEC("struct_ops/scx_llm_pin_init")
s32 BPF_PROG(scx_llm_pin_init)
{
	struct llm_pin_config *cfg;
	u32 key = 0;
	s32 ret;

	cfg = bpf_map_lookup_elem(&config_map, &key);
	if (!cfg || cfg->switch_all)
		scx_bpf_switch_all();

	#pragma unroll
	for (u32 cpu = 0; cpu < 8; cpu++) {
		ret = scx_bpf_create_dsq(vector_dsq_id((s32)cpu), -1);
		if (ret)
			return ret;
	}

	return 0;
}

SEC("struct_ops/scx_llm_pin_exit")
void BPF_PROG(scx_llm_pin_exit, struct scx_exit_info *ei)
{
	// 清理工作（如果需要）
}

SEC("struct_ops/scx_llm_pin_disable")
void BPF_PROG(scx_llm_pin_disable, struct task_struct *p)
{
	struct rvv_task_class *state;
	__u32 tid = task_pid(p);
	__u32 cpu_key;
	__u32 *owner;

	state = bpf_map_lookup_elem(&rvv_task_classes, &tid);
	if (!state)
		return;

	if (state->assigned_cpu_plus1) {
		cpu_key = state->assigned_cpu_plus1 - 1;
		if (cpu_key < 8) {
			owner = bpf_map_lookup_elem(&vector_cpu_owners, &cpu_key);
			if (owner)
				__sync_val_compare_and_swap(owner, tid, 0);
		}
	}
	bpf_map_delete_elem(&selected_cpu_map, &tid);
	bpf_map_delete_elem(&rvv_task_classes, &tid);
}

SEC(".struct_ops")
struct sched_ext_ops scx_llm_pin_ops = {
	.select_cpu		= (void *)scx_llm_pin_select_cpu,
	.enqueue		= (void *)scx_llm_pin_enqueue,
	.dispatch		= (void *)scx_llm_pin_dispatch,
	.init			= (void *)scx_llm_pin_init,
	.exit			= (void *)scx_llm_pin_exit,
	.disable		= (void *)scx_llm_pin_disable,
	.dispatch_max_batch	= 1,
	.timeout_ms		= 5000,
	.name			= "scx_llm_pin",
};
