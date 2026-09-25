// SPDX-License-Identifier: GPL-2.0
/*
 * scx_llm_pin - RVV-aware sched_ext scheduler for LLM inference workloads.
 */
#ifdef SCX_VMLINUX_HEADER
#include SCX_VMLINUX_HEADER
#else
#include "vmlinux.h"
#endif
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

// sched_ext renamed the dispatch/consume helpers in newer kernels. Keep the
// policy source shared while resolving the actual kfunc name at build time.
#ifdef SCX_API_DSQ_INSERT
void scx_bpf_dsq_insert(struct task_struct *p, u64 dsq_id, u64 slice,
			u64 enq_flags) __ksym;
bool scx_bpf_dsq_move_to_local(u64 dsq_id) __ksym;
#define scx_bpf_dispatch scx_bpf_dsq_insert
#define scx_bpf_consume scx_bpf_dsq_move_to_local
#else
void scx_bpf_dispatch(struct task_struct *p, u64 dsq_id, u64 slice,
		      u64 enq_flags) __ksym;
bool scx_bpf_consume(u64 dsq_id) __ksym;
#endif
#ifndef SCX_API_DSQ_INSERT
void scx_bpf_switch_all(void) __ksym;
#endif
s32 scx_bpf_pick_idle_cpu(const cpumask_t *cpus_allowed, u64 flags) __ksym;
s32 scx_bpf_create_dsq(u64 dsq_id, s32 node) __ksym;
void scx_bpf_kick_cpu(s32 cpu, u64 flags) __ksym;

#define VECTOR_DSQ_BASE 0x100000ULL
#define PHASE_LEASE_LONG_WAIT_NS (4000ULL * 1000 * 1000)

// 配置 map（单元素，用户态写入）
struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, 1);
	__type(key, u32);
	__type(value, struct llm_pin_config);
} config_map SEC(".maps");

// ABI v4 worker page map: created/pinned for the userspace lease runtime and
// read by user-space tools for status/validation only.
struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, RVV_LEASE_CONTROL_TEAMS);
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

// 每个 (team, CPU) 的 vector 线程所有者 TID，0 表示未分配
struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, RVV_LEASE_CONTROL_TEAMS * 8);
	__type(key, __u32);
	__type(value, __u32);
} vector_cpu_owners SEC(".maps");

// 每个 (team, CPU) 的 vector-local 窗口结束时间戳（ns）。vector DSQ 被消费后
// 打开一个 vector_local_window_ns 窗口，期间该 team 的 vector 任务改走 local
// DSQ，让普通/本地任务获得集中调度机会。
struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, RVV_LEASE_CONTROL_TEAMS * 8);
	__type(key, __u32);
	__type(value, __u64);
} vector_local_until_ns SEC(".maps");

// 每个 CPU 下次优先消费的 team，用于双模型 work-conserving 轮转。
struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, 8);
	__type(key, __u32);
	__type(value, __u32);
} vector_next_team SEC(".maps");

struct vector_pending_state {
	__u64 since_ns;
	__u64 max_wait_ns;
};

// Runnable vector work waiting outside the local lease, keyed by (team, CPU).
struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, RVV_LEASE_CONTROL_TEAMS * 8);
	__type(key, __u32);
	__type(value, struct vector_pending_state);
} vector_pending_state_map SEC(".maps");

// 每个 CPU 最近一次 phase-lease 选中的 preferred team 及其有效期。
struct phase_lease_cpu_state {
	__u32 preferred_team_plus1;
	__u32 phase_signature;
	__u64 valid_until_ns;
};

struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, 8);
	__type(key, __u32);
	__type(value, struct phase_lease_cpu_state);
} phase_lease_cpu_state_map SEC(".maps");

// 0=没有 vector worker，1..8=team 0..7。
struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, 8);
	__type(key, __u32);
	__type(value, __u32);
} running_team_by_cpu SEC(".maps");

// Keep the mask in generated BPF: older verifiers lose scalar bounds across
// helpers taking stack keys, even when LLVM proves the original bounds.
static __always_inline __u32 bounded_team(__u32 team)
{
    asm volatile("%0 &= 7" : "+r"(team));
    return team;
}

static __always_inline __u32 task_tgid(struct task_struct *p)
{
	pid_t tgid;

	bpf_probe_read_kernel(&tgid, sizeof(tgid), &p->tgid);
	return (__u32)tgid;
}

// Model identity is resolved from a bounded runtime team table.
static __always_inline s32 llm_team_id(struct task_struct *p,
                                       const struct llm_pin_config *cfg)
{
    __u32 tgid = task_tgid(p);
    if (!cfg) return -1;
    for (__u32 i = 0; i < RVV_LEASE_CONTROL_TEAMS; i++)
        if (i < cfg->team_count && cfg->teams[i].tgid == tgid) return i;
    return -1;
}

static __always_inline __u32 team_target_tgid(
    const struct llm_pin_config *cfg, __u32 team)
{
    return team < RVV_LEASE_CONTROL_TEAMS && team < cfg->team_count ?
        cfg->teams[bounded_team(team)].tgid : 0;
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

static __always_inline void release_vector_cpu(__u32 tid, __u32 team, s32 cpu);

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

static __always_inline void record_vector_thread(
	struct llm_pin_stats *stats, __u32 team)
{
	if (!stats)
		return;
	__sync_fetch_and_add(&stats->rvv_vector_threads, 1);
	if (team == 0)
		__sync_fetch_and_add(&stats->rvv_vector_threads_team0, 1);
	else if (team == 1)
		__sync_fetch_and_add(&stats->rvv_vector_threads_team1, 1);
	if (team < RVV_LEASE_CONTROL_TEAMS)
		__sync_fetch_and_add(&stats->teams[bounded_team(team)].vector_threads, 1);
}

static __always_inline void unrecord_vector_thread(
	struct llm_pin_stats *stats, __u32 team)
{
	if (!stats)
		return;
	__sync_fetch_and_sub(&stats->rvv_vector_threads, 1);
	if (team == 0)
		__sync_fetch_and_sub(&stats->rvv_vector_threads_team0, 1);
	else if (team == 1)
		__sync_fetch_and_sub(&stats->rvv_vector_threads_team1, 1);
	if (team < RVV_LEASE_CONTROL_TEAMS)
		__sync_fetch_and_sub(&stats->teams[bounded_team(team)].vector_threads, 1);
}

static __always_inline bool classify_rvv_vector_task(struct task_struct *p,
    const struct llm_pin_config *cfg, struct llm_pin_stats *stats, __u32 team)
{
    __u32 tid = task_pid(p);
    if (team >= RVV_LEASE_CONTROL_TEAMS) return false;
    struct rvv_task_class *state = get_rvv_task_class(tid);
    if (!state) return false;
    state->team_id_plus1 = team + 1;
    bool ok = false;
    unsigned long vs = read_vs_state(p, &ok);
    if (ok && vs == RVV_SR_VS_DIRTY) {
        if (state->dirty_score < 0xffffffffU) state->dirty_score++;
        if (stats) __sync_fetch_and_add(&stats->rvv_dirty_samples, 1);
    }
    if (!state->is_vector && state->dirty_score >= cfg->dirty_threshold) {
        state->is_vector = 1;
        record_vector_thread(stats, team);
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

static __always_inline __u32 team_cpu_key(__u32 team, s32 cpu)
{
	return team * 8 + (__u32)cpu;
}

static __always_inline u64 vector_dsq_id(__u32 team, s32 cpu)
{
	return VECTOR_DSQ_BASE + (u64)team_cpu_key(team, cpu);
}

// window>0 时始终启用：vector DSQ 消费后打开一个 local 窗口。
static __always_inline void start_vector_local_window(
	const struct llm_pin_config *cfg, struct llm_pin_stats *stats,
	__u32 team, s32 cpu, __u64 now)
{
	__u64 *local_until;
	__u32 key;

	if (!cfg || !cfg->vector_local_window_ns ||
	    team >= RVV_LEASE_CONTROL_TEAMS || cpu < 0 || cpu >= 8)
		return;
	key = team_cpu_key(team, cpu);
	local_until = bpf_map_lookup_elem(&vector_local_until_ns, &key);
	if (!local_until)
		return;
	*local_until = now + cfg->vector_local_window_ns;
	if (stats)
		__sync_fetch_and_add(&stats->rvv_vector_window_starts, 1);
}

static __always_inline __u64 vector_local_window_deadline(
	const struct llm_pin_config *cfg, __u32 team, s32 cpu, __u64 now)
{
	__u64 *local_until;
	__u32 key;

	if (!cfg || !cfg->vector_local_window_ns ||
	    team >= RVV_LEASE_CONTROL_TEAMS || cpu < 0 || cpu >= 8)
		return 0;
	key = team_cpu_key(team, cpu);
	local_until = bpf_map_lookup_elem(&vector_local_until_ns, &key);
	return local_until && *local_until > now ? *local_until : 0;
}

static __always_inline struct vector_pending_state *vector_pending_state(
	__u32 team, s32 cpu)
{
	__u32 key;

	if (team >= RVV_LEASE_CONTROL_TEAMS || cpu < 0 || cpu >= 8)
		return NULL;
	key = team_cpu_key(team, cpu);
	return bpf_map_lookup_elem(&vector_pending_state_map, &key);
}

// Returns true when this call made the (team,cpu) DSQ transition empty->non-empty.
static __always_inline bool mark_vector_pending(__u32 team, s32 cpu, __u64 now)
{
	struct vector_pending_state *pending;

	pending = vector_pending_state(team, cpu);
	if (!pending || pending->since_ns)
		return false;
	return __sync_val_compare_and_swap(&pending->since_ns, 0, now) == 0;
}

// A custom DSQ insert does not wake the target CPU. When we make a vector DSQ
// non-empty and the target CPU isn't this one, kick it so an idle target runs
// dispatch and consumes the task.
static __always_inline void kick_vector_target(s32 cpu)
{
	if (cpu >= 0 && cpu < 8 && cpu != bpf_get_smp_processor_id())
		scx_bpf_kick_cpu(cpu, 0);
}

static __always_inline __u64 clear_vector_pending(
	__u32 team, s32 cpu, __u64 now, struct llm_pin_stats *stats, bool stale)
{
	struct vector_pending_state *pending;
	__u64 since;
	__u64 wait_ns;

	pending = vector_pending_state(team, cpu);
	if (!pending)
		return 0;
	since = pending->since_ns;
	if (!since || __sync_val_compare_and_swap(&pending->since_ns, since, 0) != since)
		return 0;
	wait_ns = now > since ? now - since : 0;
	if (wait_ns > pending->max_wait_ns)
		pending->max_wait_ns = wait_ns;
	if (stats) {
		if (wait_ns > stats->phase_lease_pending_max_ns)
			stats->phase_lease_pending_max_ns = wait_ns;
		if (wait_ns >= PHASE_LEASE_LONG_WAIT_NS)
			__sync_fetch_and_add(&stats->phase_lease_pending_ge_4s, 1);
		if (stale)
			__sync_fetch_and_add(&stats->phase_lease_pending_stale, 1);
	}
	return wait_ns;
}

static __always_inline __u64 vector_pending_since(__u32 team, s32 cpu)
{
	struct vector_pending_state *pending;

	pending = vector_pending_state(team, cpu);
	return pending ? pending->since_ns : 0;
}

struct team_phase_snapshot {
	__u32 phase;
	__u32 active_workers;
	__u32 prompt_tokens;
	__u32 decode_tokens;
	__u64 phase_epoch;
	__u64 begin_ns;
	__u64 expires_ns;
};

static __attribute__((noinline)) void record_phase_idle(
	struct llm_pin_stats *stats)
{
	__sync_fetch_and_add(&stats->phase_hint_idle, 1);
}

static __attribute__((noinline)) void record_phase_prefill(
	struct llm_pin_stats *stats)
{
	__sync_fetch_and_add(&stats->phase_hint_prefill, 1);
}

static __attribute__((noinline)) void record_phase_decode(
	struct llm_pin_stats *stats)
{
	__sync_fetch_and_add(&stats->phase_hint_decode, 1);
}

static __attribute__((noinline)) void record_phase_mixed(
	struct llm_pin_stats *stats)
{
	__sync_fetch_and_add(&stats->phase_hint_mixed, 1);
}

static __attribute__((noinline)) bool read_team_phase(
	const struct llm_pin_config *cfg, struct llm_pin_stats *stats,
	__u32 team, __u64 now, struct team_phase_snapshot *out)
{
	struct rvv_lease_worker_page *page;
	const struct rvv_lease_team_hint *hint;
	__u32 sequence;
	__u32 key = team;

	if (stats)
		__sync_fetch_and_add(&stats->phase_hint_reads, 1);
	if (!cfg || !out || team >= RVV_LEASE_CONTROL_TEAMS)
		return false;
	page = bpf_map_lookup_elem(&user_worker_map, &key);
	if (!page || page->header.magic != RVV_LEASE_CONTROL_MAGIC ||
	    page->header.abi_version != RVV_LEASE_CONTROL_ABI_VERSION ||
	    !page->header.active || page->header.team_id != team ||
	    page->header.max_workers != RVV_LEASE_CONTROL_WORKERS ||
	    !page->header.worker_limit ||
	    page->header.worker_limit > RVV_LEASE_MAX_ACTIVE_WORKERS ||
	    page->header.target_tgid != team_target_tgid(cfg, team) ||
	    page->header.session_id != cfg->user_session_id) {
		if (stats)
			__sync_fetch_and_add(&stats->phase_hint_invalid, 1);
		return false;
	}
	hint = &page->team_hint;
	sequence = hint->sequence;
	if (!sequence || (sequence & 1U))
		return false;
	out->phase = hint->phase;
	out->active_workers = hint->active_workers;
	out->prompt_tokens = hint->prompt_tokens;
	out->decode_tokens = hint->decode_tokens;
	out->phase_epoch = hint->phase_epoch;
	out->begin_ns = hint->begin_ns;
	out->expires_ns = hint->expires_ns;
	if (sequence != hint->sequence || hint->session_id != cfg->user_session_id ||
	    out->phase > RVV_LEASE_PHASE_MIXED ||
	    out->active_workers > RVV_LEASE_MAX_ACTIVE_WORKERS ||
	    (out->phase != RVV_LEASE_PHASE_IDLE &&
	     !out->active_workers) ||
	    (out->phase != RVV_LEASE_PHASE_IDLE &&
	     (!out->expires_ns || out->expires_ns <= now)) ||
	    (out->phase != RVV_LEASE_PHASE_IDLE && cfg->phase_hint_timeout_ns &&
	     (!out->begin_ns || now - out->begin_ns > cfg->phase_hint_timeout_ns))) {
		if (stats)
			__sync_fetch_and_add(&stats->phase_hint_invalid, 1);
		return false;
	}
	if (stats) {
		__sync_fetch_and_add(&stats->phase_hint_valid, 1);
		if (out->phase == RVV_LEASE_PHASE_IDLE)
			record_phase_idle(stats);
		else if (out->phase == RVV_LEASE_PHASE_PREFILL)
			record_phase_prefill(stats);
		else if (out->phase == RVV_LEASE_PHASE_DECODE)
			record_phase_decode(stats);
		else
			record_phase_mixed(stats);
	}
	return true;
}

static __always_inline bool latency_phase(__u32 phase)
{
	return phase == RVV_LEASE_PHASE_PREFILL ||
	       phase == RVV_LEASE_PHASE_MIXED;
}

static __always_inline void clear_phase_lease_cpu_state(s32 cpu)
{
	struct phase_lease_cpu_state *state;
	__u32 key;

	if (cpu < 0 || cpu >= 8)
		return;
	key = (__u32)cpu;
	state = bpf_map_lookup_elem(&phase_lease_cpu_state_map, &key);
	if (!state)
		return;
	state->preferred_team_plus1 = 0;
	state->phase_signature = 0;
	state->valid_until_ns = 0;
}

static __always_inline bool team_latency_phase(
    const struct llm_pin_config *cfg, struct llm_pin_stats *stats,
    __u32 team, __u64 now)
{
    struct team_phase_snapshot phase = {};
    return read_team_phase(cfg, stats, team, now, &phase) && latency_phase(phase.phase);
}

static __always_inline bool try_phase_lease_wakeup_preempt(
    struct task_struct *p, __u32 task_team, s32 cpu,
    const struct llm_pin_config *cfg, struct llm_pin_stats *stats,
    __u64 now, __u64 slice, __u64 enq_flags)
{
    if (!cfg->phase_wakeup_preempt_enabled || !(enq_flags & SCX_ENQ_WAKEUP) ||
        cpu < 0 || cpu >= 8 ||
        !team_latency_phase(cfg, stats, task_team, now)) return false;
    __u32 key = cpu;
    __u32 *running = bpf_map_lookup_elem(&running_team_by_cpu, &key);
    if (!running || !*running || *running == task_team + 1 ||
        *running > cfg->team_count) return false;
    __u32 other = *running - 1;
    if (team_latency_phase(cfg, stats, other, now)) return false;
    if (stats) __sync_fetch_and_add(&stats->phase_lease_wakeup_candidates, 1);
    scx_bpf_dispatch(p, SCX_DSQ_LOCAL, slice, enq_flags | SCX_ENQ_PREEMPT);
    if (stats) __sync_fetch_and_add(&stats->phase_lease_wakeup_preempts, 1);
    return true;
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

static __always_inline bool claim_vector_cpu(__u32 tid, __u32 team, s32 cpu,
					      __u32 allowed_mask)
{
	__u32 cpu_key;
	__u32 *owner;

	if (!cpu_in_mask(cpu, allowed_mask))
		return false;

	if (team >= RVV_LEASE_CONTROL_TEAMS)
		return false;
	cpu_key = team_cpu_key(team, cpu);
	owner = bpf_map_lookup_elem(&vector_cpu_owners, &cpu_key);
	if (!owner)
		return false;
	if (*owner == tid)
		return true;
	if (*owner == 0 && __sync_val_compare_and_swap(owner, 0, tid) == 0)
		return true;
	return false;
}

static __always_inline void release_vector_cpu(__u32 tid, __u32 team, s32 cpu)
{
	__u32 cpu_key;
	__u32 *owner;

	if (team >= RVV_LEASE_CONTROL_TEAMS || cpu < 0 || cpu >= 8)
		return;
	cpu_key = team_cpu_key(team, cpu);
	owner = bpf_map_lookup_elem(&vector_cpu_owners, &cpu_key);
	if (owner)
		__sync_val_compare_and_swap(owner, tid, 0);
}

static __always_inline s32 select_unique_vector_cpu(struct task_struct *p,
						      const struct llm_pin_config *cfg,
						      struct llm_pin_stats *stats,
						      s32 prev_cpu, __u32 team)
{
	struct rvv_task_class *state;
	__u32 tid = task_pid(p);
	__u32 allowed_mask;
    if (team >= RVV_LEASE_CONTROL_TEAMS) return prev_cpu;
    const struct cpumask *mask = BPF_CORE_READ(p, cpus_ptr);
    unsigned long bits = 0;
    bpf_probe_read_kernel(&bits, sizeof(bits), &mask->bits[0]);
    allowed_mask = cfg->teams[bounded_team(team)].cpu_mask & (__u32)bits & LLM_CPU_MASK;
	s32 assigned_cpu;

	state = get_rvv_task_class(tid);
	if (!state)
		return select_cpu_from_mask(allowed_mask, prev_cpu);

	if (state->assigned_cpu_plus1) {
		assigned_cpu = (s32)state->assigned_cpu_plus1 - 1;
		if (claim_vector_cpu(tid, team, assigned_cpu, allowed_mask))
			return assigned_cpu;
		state->assigned_cpu_plus1 = 0;
	}

	if (claim_vector_cpu(tid, team, prev_cpu, allowed_mask)) {
		state->assigned_cpu_plus1 = (__u32)prev_cpu + 1;
		return prev_cpu;
	}

	#pragma unroll
	for (__u32 offset = 0; offset < 8; offset++) {
		s32 cpu = (s32)((tid + offset) & 7);

		if (claim_vector_cpu(tid, team, cpu, allowed_mask)) {
			state->assigned_cpu_plus1 = (__u32)cpu + 1;
			return cpu;
		}
	}

	return select_cpu_from_mask(allowed_mask, prev_cpu);
}

static __always_inline s32 select_rvv_vector_cpu(struct task_struct *p,
						  const struct llm_pin_config *cfg,
						  struct llm_pin_stats *stats,
						  s32 prev_cpu, __u32 team)
{
	s32 cpu;

	if (cfg->vector_local_window_ns) {
		cpu = select_unique_vector_cpu(p, cfg, stats, prev_cpu, team);
		remember_selected_cpu(p, cpu);
		if (stats && cpu != prev_cpu)
			__sync_fetch_and_add(&stats->cpu_migrations, 1);
		return cpu;
	}

	if (cpu_in_mask(prev_cpu, LLM_CPU_MASK)) {
		remember_selected_cpu(p, prev_cpu);
		return prev_cpu;
	}
	cpu = select_cpu_from_mask(LLM_CPU_MASK, prev_cpu);
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
	s32 team;
	u32 key = 0;

	cfg = bpf_map_lookup_elem(&config_map, &key);
	if (!cfg || !cfg->enabled)
		return prev_cpu;  // 默认保持在原CPU

	stats = bpf_map_lookup_elem(&stats_map, &key);
	team = llm_team_id(p, cfg);

	if (team >= 0) {
		if (classify_rvv_vector_task(p, cfg, stats, (__u32)team)) {
			s32 cpu;

			cpu = select_rvv_vector_cpu(p, cfg, stats, prev_cpu,
						    (__u32)team);
			if (stats) {
				__sync_fetch_and_add(&stats->llm_scheduled, 1);
				__sync_fetch_and_add(&stats->llm_pinned, 1);
			}
			return cpu >= 0 ? cpu : prev_cpu;
		}

		if (stats)
			__sync_fetch_and_add(&stats->llm_scheduled, 1);
		return prev_cpu;
	} else {
		if (stats)
			__sync_fetch_and_add(&stats->system_scheduled, 1);
		return prev_cpu;
	}
}

SEC("struct_ops/scx_llm_pin_enqueue")
void BPF_PROG(scx_llm_pin_enqueue, struct task_struct *p, u64 enq_flags)
{
	struct llm_pin_config *cfg;
	struct llm_pin_stats *stats;
	u64 slice = SCX_SLICE_DFL;
	s32 team;
	u32 key = 0;

	cfg = bpf_map_lookup_elem(&config_map, &key);
	stats = bpf_map_lookup_elem(&stats_map, &key);

	if (!cfg || !cfg->enabled) {
		scx_bpf_dispatch(p, SCX_DSQ_GLOBAL, SCX_SLICE_DFL, enq_flags);
		return;
	}
	team = llm_team_id(p, cfg);

	if (team >= 0) {
		if (classify_rvv_vector_task(p, cfg, stats, (__u32)team)) {
			s32 selected_cpu = remembered_selected_cpu(p);
			u64 dsq_id;
			__u64 now;

			if (cfg->vector_local_window_ns)
				selected_cpu = select_unique_vector_cpu(p, cfg, stats,
								 selected_cpu,
								 (__u32)team);
			remember_selected_cpu(p, selected_cpu);

			if (selected_cpu < 0 || selected_cpu >= 8) {
				scx_bpf_dispatch(p, SCX_DSQ_GLOBAL, SCX_SLICE_DFL, enq_flags);
				return;
			}
			now = bpf_ktime_get_ns();

			if (cfg->vector_slice_ns)
				slice = cfg->vector_slice_ns;

			if (stats)
				__sync_fetch_and_add(&stats->rvv_vector_dispatches, 1);

			if (try_phase_lease_wakeup_preempt(
					p, (__u32)team, selected_cpu, cfg, stats,
					now, slice, enq_flags))
				return;

			if (cfg->vector_local_window_ns) {
				__u64 deadline = vector_local_window_deadline(
					cfg, (__u32)team, selected_cpu, now);
				if (deadline) {
					slice = clamp_slice(slice, deadline - now, stats);
					if (stats)
						__sync_fetch_and_add(
							&stats->rvv_vector_local_dispatches, 1);
					scx_bpf_dispatch(p, SCX_DSQ_LOCAL, slice, enq_flags);
					return;
				}
				slice = clamp_slice(slice, cfg->vector_local_window_ns, stats);
			}

			if (stats) {
				__sync_fetch_and_add(&stats->rvv_vector_dsq_dispatches, 1);
				if (team == 0)
					__sync_fetch_and_add(
						&stats->rvv_vector_dsq_dispatches_team0, 1);
				else if (team == 1)
					__sync_fetch_and_add(
						&stats->rvv_vector_dsq_dispatches_team1, 1);
				if (team < RVV_LEASE_CONTROL_TEAMS)
					__sync_fetch_and_add(&stats->teams[bounded_team(team)].enqueues, 1);
			}
			dsq_id = vector_dsq_id((__u32)team, selected_cpu);
			bool started = mark_vector_pending((__u32)team, selected_cpu, now);
			scx_bpf_dispatch(p, dsq_id, slice, enq_flags);
			if (started)
				kick_vector_target(selected_cpu);
			return;
		}

		if (stats)
			__sync_fetch_and_add(&stats->rvv_nonvector_llm_dispatches, 1);
		if (stats)
			__sync_fetch_and_add(&stats->llm_local_dispatches, 1);
		scx_bpf_dispatch(p, SCX_DSQ_LOCAL, slice, enq_flags);
	} else {
		if (stats)
			__sync_fetch_and_add(&stats->system_dispatches, 1);
		scx_bpf_dispatch(p, SCX_DSQ_GLOBAL, slice, enq_flags);
	}
}

static __always_inline bool consume_vector_team(
	s32 cpu, __u32 team, const struct llm_pin_config *cfg,
	struct llm_pin_stats *stats, bool start_local_window)
{
	__u64 now;

	if (team >= RVV_LEASE_CONTROL_TEAMS ||
	    !scx_bpf_consume(vector_dsq_id(team, cpu)))
		return false;

	now = bpf_ktime_get_ns();
	clear_vector_pending(team, cpu, now, stats, false);
	if (start_local_window)
		start_vector_local_window(cfg, stats, team, cpu, now);
	if (stats) {
		__sync_fetch_and_add(&stats->rvv_vector_dsq_consumes, 1);
		if (team == 0)
			__sync_fetch_and_add(&stats->rvv_vector_dsq_consumes_team0, 1);
		else if (team == 1)
			__sync_fetch_and_add(&stats->rvv_vector_dsq_consumes_team1, 1);
		__sync_fetch_and_add(&stats->teams[bounded_team(team)].consumes, 1);
	}
	return true;
}

static __always_inline bool phase_lease_pending_overdue(
	__u64 since, __u64 now, __u64 threshold)
{
	return since && now > since && now - since >= threshold;
}

static __always_inline bool try_phase_lease_safety_team(
	s32 cpu, __u32 team, const struct llm_pin_config *cfg,
	struct llm_pin_stats *stats, __u64 now, __u64 threshold)
{
	__u64 since = vector_pending_since(team, cpu);

	(void)cfg;
	if (!phase_lease_pending_overdue(since, now, threshold))
		return false;
	if (consume_vector_team(cpu, team, cfg, stats, false)) {
		if (stats)
			__sync_fetch_and_add(&stats->phase_lease_safety_forced, 1);
		return true;
	}
	clear_vector_pending(team, cpu, now, stats, true);
	if (stats)
		__sync_fetch_and_add(&stats->phase_lease_safety_misses, 1);
	return false;
}

static __always_inline bool consume_phase_lease_safety(
    s32 cpu, const struct llm_pin_config *cfg, struct llm_pin_stats *stats, __u64 now)
{
    if (cpu < 0 || cpu >= 8) return false;
    __u64 oldest = now;
    __u32 selected = RVV_LEASE_CONTROL_TEAMS;
    for (__u32 t = 0; t < RVV_LEASE_CONTROL_TEAMS; t++) {
        if (t >= cfg->team_count) break;
        __u64 since = vector_pending_since(t, cpu);
        if (phase_lease_pending_overdue(since, now, cfg->prefill_starvation_ns) &&
            since < oldest) { oldest = since; selected = t; }
    }
    return selected < RVV_LEASE_CONTROL_TEAMS &&
        try_phase_lease_safety_team(cpu, selected, cfg, stats, now, cfg->prefill_starvation_ns);
}

SEC("struct_ops/scx_llm_pin_dispatch")
void BPF_PROG(scx_llm_pin_dispatch, s32 cpu, struct task_struct *prev)
{
    __u32 key = 0, cpu_key = cpu;
    struct llm_pin_config *cfg = bpf_map_lookup_elem(&config_map, &key);
    struct llm_pin_stats *stats = bpf_map_lookup_elem(&stats_map, &key);
    __u64 now = bpf_ktime_get_ns();
    if (cfg && consume_phase_lease_safety(cpu, cfg, stats, now)) return;
#ifndef SCX_API_DSQ_INSERT
    if (scx_bpf_consume(SCX_DSQ_GLOBAL)) return;
#endif
    if (!cfg || !cfg->enabled || cpu < 0 || cpu >= 8 ||
        !cfg->team_count || cfg->team_count > RVV_LEASE_CONTROL_TEAMS) return;
    __u32 *cursor = bpf_map_lookup_elem(&vector_next_team, &cpu_key);
    __u32 start = cursor ? *cursor : 0;
    if (start >= cfg->team_count) start = 0;
    clear_phase_lease_cpu_state(cpu);
    bool latency_candidate = false;
    // Pass 0 gives every runnable prefill/MIXED (latency) team priority.
    // Pass 1 is the ordinary work-conserving round-robin fallback.
    for (__u32 pass = 0; pass < 2; pass++) {
        for (__u32 off = 0; off < RVV_LEASE_CONTROL_TEAMS; off++) {
            if (off >= cfg->team_count) break;
            __u32 t = start + off;
            if (t >= cfg->team_count) t -= cfg->team_count;
            if (t >= RVV_LEASE_CONTROL_TEAMS) continue;
            if (!pass) {
                if (!team_latency_phase(cfg, stats, t, now)) continue;
                latency_candidate = true;
            }
            if (!consume_vector_team(cpu, t, cfg, stats, true)) continue;
            if (cursor) *cursor = t + 1 == cfg->team_count ? 0 : t + 1;
            if (!pass) {
                struct phase_lease_cpu_state *lease =
                    bpf_map_lookup_elem(&phase_lease_cpu_state_map, &cpu_key);
                if (lease) {
                    lease->preferred_team_plus1 = t + 1;
                    lease->valid_until_ns = now + cfg->vector_slice_ns;
                }
                if (stats) {
                    __sync_fetch_and_add(&stats->phase_lease_preferred_consumes, 1);
                    __sync_fetch_and_add(&stats->phase_lease_engagements, 1);
                }
            } else if (stats && latency_candidate) {
                __sync_fetch_and_add(&stats->phase_lease_fallback_consumes, 1);
            }
            return;
        }
    }
    if (stats && latency_candidate)
        __sync_fetch_and_add(&stats->phase_lease_empty_fallbacks, 1);
}

SEC("struct_ops/scx_llm_pin_running")
void BPF_PROG(scx_llm_pin_running, struct task_struct *p)
{
	struct phase_lease_cpu_state *phase_lease;
	struct llm_pin_stats *stats;
	struct rvv_task_class *state;
	__u64 now;
	__u32 team_plus1;
	__u32 tid = task_pid(p);
	__u32 cpu = (__u32)bpf_get_smp_processor_id();
	__u32 key = 0;
	__u32 *running_team;

	if (cpu >= 8)
		return;
	state = bpf_map_lookup_elem(&rvv_task_classes, &tid);
	if (!state || !state->is_vector || !state->team_id_plus1)
		return;
	team_plus1 = state->team_id_plus1;
	running_team = bpf_map_lookup_elem(&running_team_by_cpu, &cpu);
	if (running_team)
		*running_team = team_plus1;
	state->running = 1;

	stats = bpf_map_lookup_elem(&stats_map, &key);
	if (stats && team_plus1 && team_plus1 <= RVV_LEASE_CONTROL_TEAMS)
		__sync_fetch_and_add(&stats->teams[bounded_team(team_plus1 - 1)].runs, 1);
	phase_lease = bpf_map_lookup_elem(&phase_lease_cpu_state_map, &cpu);
	now = bpf_ktime_get_ns();
	if (stats && phase_lease && phase_lease->preferred_team_plus1 &&
	    phase_lease->valid_until_ns > now) {
		__sync_fetch_and_add(&stats->phase_lease_running_samples, 1);
		if (team_plus1 == phase_lease->preferred_team_plus1)
			__sync_fetch_and_add(&stats->phase_lease_preferred_runs, 1);
		else
			__sync_fetch_and_add(&stats->phase_lease_other_runs, 1);
	}
}

SEC("struct_ops/scx_llm_pin_stopping")
void BPF_PROG(scx_llm_pin_stopping, struct task_struct *p, bool runnable)
{
	struct rvv_task_class *state;
	__u32 tid = task_pid(p);
	__u32 cpu = (__u32)bpf_get_smp_processor_id();
	__u32 *running_team;

	(void)runnable;
	if (cpu >= 8)
		return;
	state = bpf_map_lookup_elem(&rvv_task_classes, &tid);
	if (!state || !state->is_vector)
		return;
	state->running = 0;
	running_team = bpf_map_lookup_elem(&running_team_by_cpu, &cpu);
	if (running_team && *running_team == state->team_id_plus1)
		*running_team = 0;
}

SEC("struct_ops/scx_llm_pin_init")
s32 BPF_PROG(scx_llm_pin_init)
{
	s32 ret;

#ifndef SCX_API_DSQ_INSERT
	scx_bpf_switch_all();
#endif

	#pragma unroll
	for (u32 team = 0; team < RVV_LEASE_CONTROL_TEAMS; team++) {
		#pragma unroll
		for (u32 cpu = 0; cpu < 8; cpu++) {
			ret = scx_bpf_create_dsq(vector_dsq_id(team, (s32)cpu), -1);
			if (ret)
				return ret;
		}
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
		__u32 team = state->team_id_plus1 ? state->team_id_plus1 - 1 : 0;

		cpu_key = team_cpu_key(team, (s32)state->assigned_cpu_plus1 - 1);
		if (team < RVV_LEASE_CONTROL_TEAMS && cpu_key <
		    RVV_LEASE_CONTROL_TEAMS * 8) {
			owner = bpf_map_lookup_elem(&vector_cpu_owners, &cpu_key);
			if (owner && __sync_val_compare_and_swap(owner, tid, 0) == tid)
				clear_vector_pending(team,
					(s32)state->assigned_cpu_plus1 - 1,
					bpf_ktime_get_ns(), NULL, false);
		}
	}
    __u32 zero = 0;
    struct llm_pin_stats *stats = bpf_map_lookup_elem(&stats_map, &zero);
    if (state->is_vector && state->team_id_plus1)
        unrecord_vector_thread(stats, state->team_id_plus1 - 1);
	bpf_map_delete_elem(&selected_cpu_map, &tid);
	bpf_map_delete_elem(&rvv_task_classes, &tid);
}

SEC(".struct_ops")
struct sched_ext_ops scx_llm_pin_ops = {
	.select_cpu		= (void *)scx_llm_pin_select_cpu,
	.enqueue		= (void *)scx_llm_pin_enqueue,
	.dispatch		= (void *)scx_llm_pin_dispatch,
	.running		= (void *)scx_llm_pin_running,
	.stopping		= (void *)scx_llm_pin_stopping,
	.init			= (void *)scx_llm_pin_init,
	.exit			= (void *)scx_llm_pin_exit,
	.disable		= (void *)scx_llm_pin_disable,
	.dispatch_max_batch	= 1,
	.timeout_ms		= 5000,
	.name			= "scx_llm_pin",
};
