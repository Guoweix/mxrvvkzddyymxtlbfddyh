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
void scx_bpf_kick_cpu(s32 cpu, u64 flags) __ksym;

#define VECTOR_DSQ_BASE 0x100000ULL
#define RVV_HINT_RETRY_NS (100ULL * 1000 * 1000)
#define RVV_HINT_REGISTRATION_GRACE_NS (1000ULL * 1000 * 1000)
#define PHASE_LEASE_LONG_WAIT_NS (4000ULL * 1000 * 1000)

// 配置 map（单元素，用户态写入）
struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, 1);
	__type(key, u32);
	__type(value, struct llm_pin_config);
} config_map SEC(".maps");

// Explicit members stop reading this map after classification. Dirty fallback
// tasks retry at a bounded rate so a late complete page can revoke false hits.
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

// 每个 (team, CPU) 当前 vector-local 窗口的结束时间戳（ns）
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

struct team_gang_state {
	struct bpf_spin_lock lock;
	__u32 active_team_plus1;
	__u32 cycle_slot;
	__u32 phase_signature;
	__u32 engaged;
	__u64 quantum_deadline_ns;
	__u64 generation;
};

struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, 1);
	__type(key, __u32);
	__type(value, struct team_gang_state);
} team_gang_state_map SEC(".maps");

// 0=没有 vector worker，1/2=team 0/1。
struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, 8);
	__type(key, __u32);
	__type(value, __u32);
} running_team_by_cpu SEC(".maps");

// Per-CPU 调度计数器（用于轮询选择）
struct {
	__uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
	__uint(max_entries, 1);
	__type(key, u32);
	__type(value, __u64);
} cpu_select_counter SEC(".maps");

static __always_inline __u32 task_tgid(struct task_struct *p)
{
	pid_t tgid;

	bpf_probe_read_kernel(&tgid, sizeof(tgid), &p->tgid);
	return (__u32)tgid;
}

// 返回 0/1 表示目标模型，-1 表示非目标任务。
static __always_inline s32 llm_team_id(struct task_struct *p,
				       const struct llm_pin_config *cfg)
{
	__u32 tgid;

	if (!cfg || !cfg->target_tgid)
		return -1;
	tgid = task_tgid(p);
	if (tgid == cfg->target_tgid)
		return 0;
	if (cfg->target_tgid2 && tgid == cfg->target_tgid2)
		return 1;
	return -1;
}

static __always_inline __u32 team_target_tgid(
	const struct llm_pin_config *cfg, __u32 team)
{
	return team == 0 ? cfg->target_tgid : cfg->target_tgid2;
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

static __always_inline bool is_phase_lease_mode(
	const struct llm_pin_config *cfg)
{
	return cfg && cfg->team_gang_mode == LLM_TEAM_GANG_PREFILL_LEASE;
}

static __always_inline bool user_preference_active(
	const struct llm_pin_config *cfg, __u64 now, __u32 *preferred_team)
{
	__u32 team_plus1;

	if (!cfg || cfg->user_control_mode != RVV_LEASE_CONTROL_ACTIVE ||
	    !cfg->target_tgid2 || !cfg->user_preferred_until_ns ||
	    cfg->user_preferred_until_ns <= now)
		return false;
	team_plus1 = cfg->user_preferred_team_plus1;
	if (!team_plus1 || team_plus1 > RVV_LEASE_CONTROL_TEAMS)
		return false;
	*preferred_team = team_plus1 - 1;
	return true;
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

// 2=complete page and explicit non-member, 1=registered worker,
// 0=valid but incomplete page, -1=invalid/unavailable page.
static __always_inline int apply_user_worker_hint(
	const struct llm_pin_config *cfg, struct llm_pin_stats *stats,
	struct rvv_task_class *state, __u32 tid, __u32 team)
{
	struct rvv_lease_worker_page *page;
	struct lease_worker_snapshot slot = {};
	__u32 allowed_mask;
	__u32 target_tgid;
	__u32 key = team;
	__u32 cpu;
	__u32 valid_slots = 0;

	if (!cfg || !state || team >= RVV_LEASE_CONTROL_TEAMS ||
	    cfg->user_control_mode == RVV_LEASE_CONTROL_OFF)
		return -1;
	target_tgid = team_target_tgid(cfg, team);
	if (stats)
		__sync_fetch_and_add(&stats->user_hint_classification_reads, 1);
	page = bpf_map_lookup_elem(&user_worker_map, &key);
	if (!page || page->header.magic != RVV_LEASE_CONTROL_MAGIC ||
	    page->header.abi_version != RVV_LEASE_CONTROL_ABI_VERSION ||
	    !page->header.active || page->header.team_id != team ||
	    page->header.target_tgid != target_tgid ||
	    page->header.max_workers != RVV_LEASE_CONTROL_WORKERS ||
	    !cfg->user_session_id ||
	    page->header.session_id != cfg->user_session_id) {
		if (stats)
			__sync_fetch_and_add(&stats->user_hint_fallbacks, 1);
		return -1;
	}

#define TRY_WORKER_SLOT(index) do {                                           \
		if (!read_lease_worker_slot(page, (index), &slot))               \
			break;                                                    \
		if (slot.tgid != target_tgid || slot.ith != (index) ||            \
		    slot.n_threads != RVV_LEASE_CONTROL_WORKERS ||                \
		    slot.session_id != cfg->user_session_id) {                    \
			if (slot.tid == tid && stats)                               \
				__sync_fetch_and_add(&stats->user_hint_invalid_workers, 1); \
			break;                                                        \
		}                                                                  \
		valid_slots++;                                                     \
		if (slot.tid != tid)                                             \
			break;                                                    \
		state->user_ith_plus1 = (index) + 1;                               \
		state->user_session_id = slot.session_id;                          \
		state->user_hint_excluded = 0;                                     \
		if (stats)                                                         \
			__sync_fetch_and_add(&stats->user_hint_valid_workers, 1);     \
		if (cfg->user_control_mode == RVV_LEASE_CONTROL_OBSERVE) {         \
			if (stats)                                                     \
				__sync_fetch_and_add(&stats->user_hint_observed_workers, 1); \
			return 1;                                                     \
		}                                                                  \
		if (stats)                                                         \
			__sync_fetch_and_add(&stats->user_hint_active_workers, 1);     \
		if (!cfg->user_cpu_map_enabled)                                    \
			return 1;                                                     \
		cpu = cfg->user_cpu_map[(index)];                                  \
		allowed_mask = cfg->vector_cpu_mask | cfg->ordinary_cpu_mask;      \
		if (!allowed_mask)                                                 \
			allowed_mask = cfg->llm_cpu_mask;                            \
		if (!cpu_in_mask((s32)cpu, allowed_mask)) {                        \
			if (stats)                                                     \
				__sync_fetch_and_add(&stats->user_hint_invalid_workers, 1); \
			return -1;                                                    \
		}                                                                  \
		state->assigned_cpu_plus1 = cpu + 1;                               \
		state->user_cpu_forced = 1;                                       \
		if (stats)                                                         \
			__sync_fetch_and_add(&stats->user_hint_active_assignments, 1); \
		return 1;                                                          \
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
		__sync_fetch_and_add(&stats->user_hint_member_misses, 1);
	if (valid_slots == RVV_LEASE_CONTROL_WORKERS) {
		state->user_hint_excluded = 1;
		state->user_session_id = cfg->user_session_id;
		if (stats)
			__sync_fetch_and_add(&stats->user_hint_excluded_tasks, 1);
		return 2;
	}
	return 0;
}

static __always_inline void record_vector_thread(
	struct llm_pin_stats *stats, __u32 team, bool primary_hint)
{
	if (!stats)
		return;
	__sync_fetch_and_add(&stats->rvv_vector_threads, 1);
	if (team == 0)
		__sync_fetch_and_add(&stats->rvv_vector_threads_team0, 1);
	else
		__sync_fetch_and_add(&stats->rvv_vector_threads_team1, 1);
	if (primary_hint)
		__sync_fetch_and_add(&stats->user_hint_primary_workers, 1);
}

static __always_inline void unrecord_vector_thread(
	struct llm_pin_stats *stats, __u32 team)
{
	if (!stats)
		return;
	__sync_fetch_and_sub(&stats->rvv_vector_threads, 1);
	if (team == 0)
		__sync_fetch_and_sub(&stats->rvv_vector_threads_team0, 1);
	else
		__sync_fetch_and_sub(&stats->rvv_vector_threads_team1, 1);
}

static __always_inline bool classify_rvv_vector_task(struct task_struct *p,
						     const struct llm_pin_config *cfg,
						     struct llm_pin_stats *stats,
						     __u32 team)
{
	struct rvv_task_class *state;
	__u32 tid = task_pid(p);
	unsigned long vs;
	bool ok = false;
	__u32 threshold;
	__u64 now;
	int hint_result = 0;

	if (!is_rvv_dirty_strategy(cfg->strategy) ||
	    team >= RVV_LEASE_CONTROL_TEAMS)
		return false;

	state = get_rvv_task_class(tid);
	if (!state)
		return false;
	if (!state->team_id_plus1)
		state->team_id_plus1 = team + 1;
	else if (state->team_id_plus1 != team + 1)
		return false;

	now = bpf_ktime_get_ns();
	if (state->is_vector && !state->user_hint_primary &&
	    cfg->user_control_mode != RVV_LEASE_CONTROL_OFF &&
	    now >= state->user_hint_retry_after_ns) {
		hint_result = apply_user_worker_hint(cfg, stats, state, tid, team);
		if (hint_result == 1) {
			state->user_hint_primary = 1;
			return true;
		}
		if (hint_result == 2) {
			if (state->assigned_cpu_plus1)
				release_vector_cpu(tid, team,
					(s32)state->assigned_cpu_plus1 - 1);
			state->is_vector = 0;
			state->dirty_score = 0;
			state->assigned_cpu_plus1 = 0;
			state->user_cpu_forced = 0;
			state->user_hint_retry_after_ns = now + RVV_HINT_RETRY_NS;
			unrecord_vector_thread(stats, team);
			if (stats)
				__sync_fetch_and_add(&stats->user_hint_demotions, 1);
			return false;
		}
		state->user_hint_retry_after_ns = now + RVV_HINT_RETRY_NS;
	}
	if (!state->is_vector && state->user_hint_excluded &&
	    state->user_session_id == cfg->user_session_id &&
	    now < state->user_hint_retry_after_ns)
		return false;
	if (!state->is_vector && cfg->user_control_mode != RVV_LEASE_CONTROL_OFF &&
	    now >= state->user_hint_retry_after_ns) {
		hint_result = apply_user_worker_hint(cfg, stats, state, tid, team);
		if (hint_result == 1) {
			state->is_vector = 1;
			state->user_hint_primary = 1;
			record_vector_thread(stats, team, true);
			return true;
		}
		if (hint_result == 2) {
			state->user_hint_retry_after_ns = now + RVV_HINT_RETRY_NS;
			return false;
		}
		if (hint_result == 0) {
			if (!state->user_hint_first_seen_ns)
				state->user_hint_first_seen_ns = now;
			if (now - state->user_hint_first_seen_ns <
			    RVV_HINT_REGISTRATION_GRACE_NS) {
				state->user_hint_retry_after_ns = now + RVV_HINT_RETRY_NS;
				if (stats)
					__sync_fetch_and_add(&stats->user_hint_deferred_tasks, 1);
				return false;
			}
		}
		state->user_hint_retry_after_ns = now + RVV_HINT_RETRY_NS;
	}

	vs = read_vs_state(p, &ok);
	if (ok && vs == RVV_SR_VS_DIRTY) {
		if (state->dirty_score < 0xffffffffU)
			state->dirty_score++;
		if (stats)
			__sync_fetch_and_add(&stats->rvv_dirty_samples, 1);
	}

	threshold = cfg->dirty_threshold ? cfg->dirty_threshold : 2;
	if (!state->is_vector && state->dirty_score >= threshold) {
		if (hint_result != 1)
			hint_result = apply_user_worker_hint(cfg, stats, state, tid, team);
		if (hint_result == 2) {
			state->dirty_score = 0;
			state->user_hint_retry_after_ns = now + RVV_HINT_RETRY_NS;
			return false;
		}
		if (hint_result != 1 && stats)
			__sync_fetch_and_add(&stats->user_hint_fallbacks, 1);
		state->is_vector = 1;
		state->user_hint_primary = hint_result == 1;
		record_vector_thread(stats, team, hint_result == 1);
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

static __always_inline struct vector_pending_state *vector_pending_state(
	__u32 team, s32 cpu)
{
	__u32 key;

	if (team >= RVV_LEASE_CONTROL_TEAMS || cpu < 0 || cpu >= 8)
		return NULL;
	key = team_cpu_key(team, cpu);
	return bpf_map_lookup_elem(&vector_pending_state_map, &key);
}

static __always_inline void mark_vector_pending(__u32 team, s32 cpu, __u64 now)
{
	struct vector_pending_state *pending;

	pending = vector_pending_state(team, cpu);
	if (!pending || pending->since_ns)
		return;
	__sync_val_compare_and_swap(&pending->since_ns, 0, now);
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

static __always_inline bool read_team_phase(
	const struct llm_pin_config *cfg, struct llm_pin_stats *stats,
	__u32 team, __u64 now, struct team_phase_snapshot *out)
{
	struct rvv_lease_worker_page *page;
	const struct rvv_lease_team_hint *hint;
	__u32 sequence;
	__u32 key = team;

	if (stats)
		__sync_fetch_and_add(&stats->phase_hint_reads, 1);
	if (!cfg || !out || team >= RVV_LEASE_CONTROL_TEAMS ||
	    cfg->user_control_mode == RVV_LEASE_CONTROL_OFF)
		return false;
	page = bpf_map_lookup_elem(&user_worker_map, &key);
	if (!page || page->header.magic != RVV_LEASE_CONTROL_MAGIC ||
	    page->header.abi_version != RVV_LEASE_CONTROL_ABI_VERSION ||
	    !page->header.active || page->header.team_id != team ||
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
	    out->active_workers > RVV_LEASE_CONTROL_WORKERS ||
	    (out->phase != RVV_LEASE_PHASE_IDLE &&
	     out->active_workers != RVV_LEASE_CONTROL_WORKERS) ||
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

static __always_inline bool resolve_phase_lease_team(
	const struct llm_pin_config *cfg, struct llm_pin_stats *stats,
	s32 cpu, __u64 now, __u32 *preferred_team)
{
	struct team_phase_snapshot phase0 = {};
	struct team_phase_snapshot phase1 = {};
	struct phase_lease_cpu_state *state;
	__u64 valid_until;
	__u32 signature;
	__u32 key;
	bool latency0;
	bool latency1;

	if (!is_phase_lease_mode(cfg) || cpu < 0 || cpu >= 8 ||
	    !read_team_phase(cfg, stats, 0, now, &phase0) ||
	    !read_team_phase(cfg, stats, 1, now, &phase1)) {
		clear_phase_lease_cpu_state(cpu);
		return false;
	}
	latency0 = latency_phase(phase0.phase);
	latency1 = latency_phase(phase1.phase);
	if (latency0 == latency1) {
		clear_phase_lease_cpu_state(cpu);
		return false;
	}

	*preferred_team = latency0 ? 0 : 1;
	valid_until = latency0 ? phase0.expires_ns : phase1.expires_ns;
	if (cfg->vector_local_window_ns &&
	    valid_until > now + cfg->vector_local_window_ns)
		valid_until = now + cfg->vector_local_window_ns;
	signature = phase0.phase | (phase1.phase << 8);
	key = (__u32)cpu;
	state = bpf_map_lookup_elem(&phase_lease_cpu_state_map, &key);
	if (state) {
		if (state->preferred_team_plus1 != *preferred_team + 1 ||
		    state->phase_signature != signature) {
			if (stats)
				__sync_fetch_and_add(&stats->phase_lease_engagements, 1);
		}
		state->preferred_team_plus1 = *preferred_team + 1;
		state->phase_signature = signature;
		state->valid_until_ns = valid_until;
	}
	return true;
}

static __always_inline bool try_phase_lease_wakeup_preempt(
	struct task_struct *p, __u32 task_team, s32 cpu,
	const struct llm_pin_config *cfg, struct llm_pin_stats *stats,
	__u64 now, __u64 slice, __u64 enq_flags)
{
	__u32 preferred_team;
	__u32 *running_team;
	__u32 cpu_key;

	if (!(enq_flags & SCX_ENQ_WAKEUP) || !is_phase_lease_mode(cfg) ||
	    task_team >= RVV_LEASE_CONTROL_TEAMS || cpu < 0 || cpu >= 8)
		return false;
	if (!resolve_phase_lease_team(cfg, stats, cpu, now, &preferred_team) ||
	    preferred_team != task_team)
		return false;
	if (stats)
		__sync_fetch_and_add(&stats->phase_lease_wakeup_candidates, 1);

	cpu_key = (__u32)cpu;
	running_team = bpf_map_lookup_elem(&running_team_by_cpu, &cpu_key);
	if (!running_team || *running_team != (task_team ^ 1U) + 1)
		return false;

	scx_bpf_dispatch(p, SCX_DSQ_LOCAL, slice,
			 enq_flags | SCX_ENQ_PREEMPT);
	if (stats)
		__sync_fetch_and_add(&stats->phase_lease_wakeup_preempts, 1);
	return true;
}

static __always_inline void clear_gang_state(void)
{
	struct team_gang_state *state;
	__u32 key = 0;

	state = bpf_map_lookup_elem(&team_gang_state_map, &key);
	if (!state)
		return;
	bpf_spin_lock(&state->lock);
	state->engaged = 0;
	state->active_team_plus1 = 0;
	state->quantum_deadline_ns = 0;
	bpf_spin_unlock(&state->lock);
}

// Returns true when dual-team gang scheduling is active. kick_all is set only
// by the CPU which starts a gang or changes the shared active team.
static __always_inline bool resolve_gang_team(
	const struct llm_pin_config *cfg, struct llm_pin_stats *stats,
	__u64 now, __u32 *active_team, bool *kick_all)
{
	struct team_phase_snapshot phase0 = {};
	struct team_phase_snapshot phase1 = {};
	struct team_gang_state *state;
	__u32 signature;
	__u32 old_team_plus1;
	__u32 next_team;
	__u32 key = 0;
	bool reset;
	bool advance;

	*kick_all = false;
	if (!cfg || (cfg->team_gang_mode != LLM_TEAM_GANG_OBSERVE &&
		     cfg->team_gang_mode != LLM_TEAM_GANG_ALWAYS) ||
	    !cfg->target_tgid2 || !read_team_phase(cfg, stats, 0, now, &phase0) ||
	    !read_team_phase(cfg, stats, 1, now, &phase1) ||
	    phase0.phase == RVV_LEASE_PHASE_IDLE ||
	    phase1.phase == RVV_LEASE_PHASE_IDLE) {
		clear_gang_state();
		return false;
	}
	if (cfg->team_gang_mode == LLM_TEAM_GANG_OBSERVE) {
		clear_gang_state();
		return false;
	}

	state = bpf_map_lookup_elem(&team_gang_state_map, &key);
	if (!state)
		return false;
	signature = phase0.phase | (phase1.phase << 8);

	bpf_spin_lock(&state->lock);
	old_team_plus1 = state->active_team_plus1;
	reset = !state->engaged || state->phase_signature != signature ||
		!state->quantum_deadline_ns;
	advance = !reset && now >= state->quantum_deadline_ns;
	if (reset) {
		state->engaged = 1;
		state->phase_signature = signature;
		state->cycle_slot = 0;
		next_team = 0;
		state->active_team_plus1 = next_team + 1;
		state->quantum_deadline_ns = now + cfg->gang_quantum_ns;
		state->generation++;
	} else if (advance) {
		next_team = state->active_team_plus1 == 1 ? 1 : 0;
		state->active_team_plus1 = next_team + 1;
		state->quantum_deadline_ns = now + cfg->gang_quantum_ns;
		state->generation++;
	}
	*active_team = state->active_team_plus1 ? state->active_team_plus1 - 1 : 0;
	*kick_all = reset ||
		(advance && old_team_plus1 != state->active_team_plus1);
	bpf_spin_unlock(&state->lock);

	if (stats && reset)
		__sync_fetch_and_add(&stats->gang_engagements, 1);
	if (stats && (reset || advance))
		__sync_fetch_and_add(&stats->gang_quanta, 1);
	if (stats && advance && old_team_plus1 != *active_team + 1)
		__sync_fetch_and_add(&stats->gang_team_switches, 1);
	return true;
}

static __always_inline void kick_all_gang_cpus(
	const struct llm_pin_config *cfg, struct llm_pin_stats *stats)
{
	#pragma unroll
	for (__u32 cpu = 0; cpu < 8; cpu++) {
		if (!(cfg->llm_cpu_mask & (1U << cpu)))
			continue;
		scx_bpf_kick_cpu((s32)cpu, SCX_KICK_PREEMPT);
		if (stats)
			__sync_fetch_and_add(&stats->gang_cpu_kicks, 1);
	}
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
								tid, team,
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

static __always_inline __u64 vector_threads_for_team(
	const struct llm_pin_stats *stats, __u32 team)
{
	if (!stats)
		return 0;
	return team == 0 ? stats->rvv_vector_threads_team0 :
			   stats->rvv_vector_threads_team1;
}

static __always_inline s32 select_rvv_vector_cpu(struct task_struct *p,
						  const struct llm_pin_config *cfg,
						  struct llm_pin_stats *stats,
						  s32 prev_cpu, __u32 team)
{
	__u32 aggressive_mask = cfg->vector_cpu_mask ? cfg->vector_cpu_mask : cfg->llm_cpu_mask;
	__u32 ordinary_mask = cfg->ordinary_cpu_mask;
	__u32 selected_mask = aggressive_mask;
	__u32 key = 0;
	__u64 *counter;
	s32 cpu;

	if (cfg->vector_local_window_ns) {
		cpu = select_unique_vector_cpu(p, cfg, stats, prev_cpu, team);
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
	s32 team;
	u32 key = 0;

	cfg = bpf_map_lookup_elem(&config_map, &key);
	if (!cfg || !cfg->enabled)
		return prev_cpu;  // 默认保持在原CPU

	if (cfg->strategy == LLM_STRATEGY_GLOBAL)
		return prev_cpu;

	stats = bpf_map_lookup_elem(&stats_map, &key);
	team = llm_team_id(p, cfg);

	if (team >= 0) {
		s32 cpu;

		if (is_rvv_dirty_strategy(cfg->strategy) &&
		    classify_rvv_vector_task(p, cfg, stats, (__u32)team)) {
			cpu = select_rvv_vector_cpu(p, cfg, stats, prev_cpu,
						    (__u32)team);
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
	s32 team;
	u32 key = 0;

	cfg = bpf_map_lookup_elem(&config_map, &key);
	stats = bpf_map_lookup_elem(&stats_map, &key);

	if (!cfg || !cfg->enabled || cfg->strategy == LLM_STRATEGY_GLOBAL) {
		scx_bpf_dispatch(p, SCX_DSQ_GLOBAL, SCX_SLICE_DFL, enq_flags);
		return;
	}
	team = llm_team_id(p, cfg);

	if (team >= 0) {
		if (is_rvv_dirty_strategy(cfg->strategy) &&
		    classify_rvv_vector_task(p, cfg, stats, (__u32)team)) {
				s32 selected_cpu = remembered_selected_cpu(p);
				u64 dsq_id;
				__u32 cpu_key;
				__u32 gang_team = 0;
				__u64 *local_until;
				__u64 now;
				bool gang_kick = false;
				bool gang_engaged = false;

			if (cfg->vector_local_window_ns)
				selected_cpu = select_unique_vector_cpu(p, cfg, stats,
								 selected_cpu,
								 (__u32)team);
			remember_selected_cpu(p, selected_cpu);

			if (selected_cpu < 0 || selected_cpu >= 8)
				selected_cpu = (s32)bpf_get_smp_processor_id();
			if (selected_cpu < 0 || selected_cpu >= 8)
				selected_cpu = 0;
			now = bpf_ktime_get_ns();

			if (cfg->vector_local_window_ns && stats &&
			    vector_threads_for_team(stats, (__u32)team) <
				    vector_unique_target(cfg)) {
				cpu_key = team_cpu_key((__u32)team, selected_cpu);
				local_until = bpf_map_lookup_elem(
					&vector_local_until_ns, &cpu_key);
				if (local_until)
					*local_until = now +
						       cfg->vector_local_window_ns;
				__sync_fetch_and_add(&stats->rvv_vector_identify_barrier_dispatches,
						     1);
				scx_bpf_dispatch(p, SCX_DSQ_LOCAL, SCX_SLICE_DFL,
						 enq_flags);
				return;
			}

			if (cfg->team_gang_mode == LLM_TEAM_GANG_OBSERVE ||
			    cfg->team_gang_mode == LLM_TEAM_GANG_ALWAYS)
				gang_engaged = resolve_gang_team(
					cfg, stats, now, &gang_team, &gang_kick);
			if (gang_kick)
				kick_all_gang_cpus(cfg, stats);
			if (gang_engaged) {
				slice = cfg->gang_quantum_ns;
				if (stats)
					__sync_fetch_and_add(&stats->gang_vector_enqueues, 1);
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
			if (gang_engaged)
				slice = clamp_slice(slice, cfg->gang_quantum_ns, stats);
			if (try_phase_lease_wakeup_preempt(
					p, (__u32)team, selected_cpu, cfg, stats,
					now, slice, enq_flags))
				return;

			if (cfg->vector_local_window_ns && !gang_engaged) {
				cpu_key = team_cpu_key((__u32)team, selected_cpu);
				local_until = bpf_map_lookup_elem(&vector_local_until_ns,
								  &cpu_key);
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
			if (stats) {
				if (team == 0)
					__sync_fetch_and_add(
						&stats->rvv_vector_dsq_dispatches_team0, 1);
				else
					__sync_fetch_and_add(
						&stats->rvv_vector_dsq_dispatches_team1, 1);
			}
			dsq_id = vector_dsq_id((__u32)team, selected_cpu);
			if (is_phase_lease_mode(cfg))
				mark_vector_pending((__u32)team, selected_cpu, now);
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

static __always_inline bool consume_vector_team(
	s32 cpu, __u32 team, const struct llm_pin_config *cfg,
	struct llm_pin_stats *stats, bool start_local_lease)
{
	__u32 cpu_key;
	__u64 *local_until;
	__u64 now;

	if (team >= RVV_LEASE_CONTROL_TEAMS ||
	    !scx_bpf_consume(vector_dsq_id(team, cpu)))
		return false;

	now = bpf_ktime_get_ns();
	clear_vector_pending(team, cpu, now, stats, false);
	cpu_key = team_cpu_key(team, cpu);
	if (start_local_lease && cfg && cfg->vector_local_window_ns) {
		local_until = bpf_map_lookup_elem(&vector_local_until_ns, &cpu_key);
		if (local_until) {
			*local_until = now + cfg->vector_local_window_ns;
			if (stats)
				__sync_fetch_and_add(&stats->rvv_vector_window_starts, 1);
		}
	}
	if (stats) {
		__sync_fetch_and_add(&stats->rvv_vector_dsq_consumes, 1);
		if (team == 0)
			__sync_fetch_and_add(&stats->rvv_vector_dsq_consumes_team0, 1);
		else
			__sync_fetch_and_add(&stats->rvv_vector_dsq_consumes_team1, 1);
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
	s32 cpu, const struct llm_pin_config *cfg,
	struct llm_pin_stats *stats, __u64 now)
{
	__u64 threshold;
	__u64 since0;
	__u64 since1;
	__u32 first;

	if (!is_phase_lease_mode(cfg) || cpu < 0 || cpu >= 8)
		return false;
	threshold = cfg->prefill_starvation_ns;
	if (!threshold)
		threshold = 1000ULL * 1000 * 1000;
	since0 = vector_pending_since(0, cpu);
	since1 = vector_pending_since(1, cpu);
	if (!phase_lease_pending_overdue(since0, now, threshold) &&
	    !phase_lease_pending_overdue(since1, now, threshold))
		return false;
	if (!phase_lease_pending_overdue(since1, now, threshold))
		first = 0;
	else if (!phase_lease_pending_overdue(since0, now, threshold))
		first = 1;
	else
		first = since0 <= since1 ? 0 : 1;

	if (try_phase_lease_safety_team(cpu, first, cfg, stats, now, threshold))
		return true;
	return try_phase_lease_safety_team(cpu, first ^ 1U, cfg, stats,
					   now, threshold);
}

SEC("struct_ops/scx_llm_pin_dispatch")
void BPF_PROG(scx_llm_pin_dispatch, s32 cpu, struct task_struct *prev)
{
	struct llm_pin_config *cfg;
	struct llm_pin_stats *stats;
	__u32 *next_team;
	__u32 cpu_key;
	__u32 first_team;
	__u32 preferred_team;
	__u32 phase_team;
	__u32 gang_team = 0;
	__u64 now;
	bool gang_kick = false;
	u32 key = 0;

	cfg = bpf_map_lookup_elem(&config_map, &key);
	stats = bpf_map_lookup_elem(&stats_map, &key);
	now = bpf_ktime_get_ns();
	if (cfg && is_rvv_dirty_strategy(cfg->strategy) &&
	    consume_phase_lease_safety(cpu, cfg, stats, now))
		return;
	if (scx_bpf_consume(SCX_DSQ_GLOBAL))
		return;

	if (cpu < 0 || cpu >= 8)
		return;
	if (!cfg || !is_rvv_dirty_strategy(cfg->strategy))
		return;
	if ((cfg->team_gang_mode == LLM_TEAM_GANG_OBSERVE ||
	     cfg->team_gang_mode == LLM_TEAM_GANG_ALWAYS) &&
	    resolve_gang_team(cfg, stats, now, &gang_team, &gang_kick)) {
		if (gang_kick)
			kick_all_gang_cpus(cfg, stats);
		if (consume_vector_team(cpu, gang_team, cfg, stats, false)) {
			if (stats)
				__sync_fetch_and_add(&stats->gang_selected_consumes, 1);
			return;
		}
		if (stats)
			__sync_fetch_and_add(&stats->gang_empty_fallbacks, 1);
		if (consume_vector_team(cpu, gang_team ^ 1U, cfg, stats, false) && stats)
			__sync_fetch_and_add(&stats->gang_fallback_consumes, 1);
		return;
	}
	if (user_preference_active(cfg, now, &preferred_team)) {
		if (consume_vector_team(cpu, preferred_team, cfg, stats, true)) {
			if (stats)
				__sync_fetch_and_add(
					&stats->user_preference_dsq_consumes, 1);
			return;
		}
		if (stats)
			__sync_fetch_and_add(&stats->user_preference_empty_fallbacks, 1);
		consume_vector_team(cpu, preferred_team ^ 1U, cfg, stats, true);
		return;
	}
	if (resolve_phase_lease_team(cfg, stats, cpu, now, &phase_team)) {
		cpu_key = (__u32)cpu;
		next_team = bpf_map_lookup_elem(&vector_next_team, &cpu_key);
		if (consume_vector_team(cpu, phase_team, cfg, stats, true)) {
			if (stats)
				__sync_fetch_and_add(
					&stats->phase_lease_preferred_consumes, 1);
			if (next_team)
				*next_team = phase_team ^ 1U;
			return;
		}
		if (stats)
			__sync_fetch_and_add(&stats->phase_lease_empty_fallbacks, 1);
		if (consume_vector_team(cpu, phase_team ^ 1U, cfg, stats, true)) {
			if (stats)
				__sync_fetch_and_add(
					&stats->phase_lease_fallback_consumes, 1);
			if (next_team)
				*next_team = phase_team;
		}
		return;
	}

	if (!cfg->target_tgid2) {
		consume_vector_team(cpu, 0, cfg, stats, true);
		return;
	}

	cpu_key = (__u32)cpu;
	next_team = bpf_map_lookup_elem(&vector_next_team, &cpu_key);
	first_team = next_team ? (*next_team & 1U) : 0;
	if (consume_vector_team(cpu, first_team, cfg, stats, true)) {
		if (next_team)
			*next_team = first_team ^ 1U;
		return;
	}
	first_team ^= 1U;
	if (consume_vector_team(cpu, first_team, cfg, stats, true) && next_team)
		*next_team = first_team ^ 1U;
}

SEC("struct_ops/scx_llm_pin_running")
void BPF_PROG(scx_llm_pin_running, struct task_struct *p)
{
	struct team_gang_state *gang;
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
	gang = bpf_map_lookup_elem(&team_gang_state_map, &key);
	if (!stats || !gang || !gang->engaged || !gang->active_team_plus1)
		return;
	__sync_fetch_and_add(&stats->gang_running_samples, 1);
	if (team_plus1 == gang->active_team_plus1)
		__sync_fetch_and_add(&stats->gang_active_team_runs, 1);
	else
		__sync_fetch_and_add(&stats->gang_wrong_team_runs, 1);
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
	struct llm_pin_config *cfg;
	u32 key = 0;
	s32 ret;

	cfg = bpf_map_lookup_elem(&config_map, &key);
	if (!cfg || cfg->switch_all)
		scx_bpf_switch_all();

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
