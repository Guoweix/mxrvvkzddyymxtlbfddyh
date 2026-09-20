// SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause
#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
#include <bpf/bpf_core_read.h>
#include "rvv_sched_cost.h"

#ifndef BPF_ANY
#define BPF_ANY		0
#endif
#ifndef BPF_NOEXIST
#define BPF_NOEXIST	1
#endif
#ifndef BPF_F_USER_STACK
#define BPF_F_USER_STACK	(1ULL << 8)
#endif

#define RVV_NR_FUTEX		98
#define RVV_NR_FUTEX_TIME64	422
#define RVV_NR_FUTEX_WAITV	449

#define RVV_FUTEX_WAIT		0
#define RVV_FUTEX_WAKE		1
#define RVV_FUTEX_REQUEUE	3
#define RVV_FUTEX_CMP_REQUEUE	4
#define RVV_FUTEX_WAKE_OP	5
#define RVV_FUTEX_LOCK_PI	6
#define RVV_FUTEX_UNLOCK_PI	7
#define RVV_FUTEX_TRYLOCK_PI	8
#define RVV_FUTEX_WAIT_BITSET	9
#define RVV_FUTEX_WAKE_BITSET	10
#define RVV_FUTEX_WAIT_REQUEUE_PI 11
#define RVV_FUTEX_CMP_REQUEUE_PI 12
#define RVV_FUTEX_LOCK_PI2	13
#define RVV_FUTEX_CMD_MASK	0x7f

#define RVV_EINTR		4
#define RVV_EAGAIN		11
#define RVV_ETIMEDOUT		110
#define RVV_ERESTARTSYS		512

char LICENSE[] SEC("license") = "Dual BSD/GPL";

struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, 1);
	__type(key, __u32);
	__type(value, struct rvv_config);
} cfg_map SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, 1);
	__type(key, __u32);
	__type(value, struct rvv_global_stats);
} global_stats SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__uint(max_entries, 1024);
	__type(key, struct rvv_task_key);
	__type(value, struct rvv_thread_stats);
} thread_stats SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__uint(max_entries, 4096);
	__type(key, struct rvv_task_key);
	__type(value, struct rvv_task_ts);
} task_ts SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__uint(max_entries, 8192);
	__type(key, struct rvv_waker_key);
	__type(value, struct rvv_waker_stats);
} waker_stats SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__uint(max_entries, 4096);
	__type(key, struct rvv_futex_addr_key);
	__type(value, struct rvv_futex_addr_stats);
} futex_addr_stats SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__uint(max_entries, 16384);
	__type(key, struct rvv_futex_tid_key);
	__type(value, struct rvv_futex_tid_stats);
} futex_tid_stats SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__uint(max_entries, 8192);
	__type(key, struct rvv_futex_stack_key);
	__type(value, struct rvv_futex_stack_stats);
} futex_stack_stats SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__uint(max_entries, 65536);
	__type(key, struct rvv_futex_addr_hist_key);
	__type(value, __u64);
} futex_addr_hists SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__uint(max_entries, 4096);
	__type(key, struct rvv_task_key);
	__type(value, struct rvv_futex_wait_inflight);
} futex_wait_inflight SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_STACK_TRACE);
	__uint(max_entries, 4096);
	__uint(key_size, sizeof(__u32));
	__uint(value_size, RVV_STACK_DEPTH * sizeof(__u64));
} futex_user_stacks SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, RVV_HIST_MAX);
	__type(key, __u32);
	__type(value, struct rvv_hist);
} hists SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, RVV_MAX_CPUS);
	__type(key, __u32);
	__type(value, struct rvv_cpu_stats);
} cpu_stats SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
	__uint(max_entries, 1);
	__type(key, __u32);
	__type(value, struct rvv_thread_stats);
} scratch_thread_stats SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
	__uint(max_entries, 1);
	__type(key, __u32);
	__type(value, struct rvv_futex_addr_stats);
} scratch_futex_addr_stats SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
	__uint(max_entries, 1);
	__type(key, __u32);
	__type(value, struct rvv_futex_tid_stats);
} scratch_futex_tid_stats SEC(".maps");

static __always_inline void inc_u64(__u64 *p)
{
	__sync_fetch_and_add(p, 1);
}

static __always_inline void add_u64(__u64 *p, __u64 v)
{
	__sync_fetch_and_add(p, v);
}

static __always_inline void update_max(__u64 *p, __u64 v)
{
	if (v > *p)
		*p = v;
}

static __always_inline __u32 hist_bucket_us(__u64 ns)
{
	__u64 us = ns / 1000;
	__u32 bucket = 1;

	if (!us)
		return 0;

#pragma unroll
	for (int i = 0; i < RVV_HIST_BUCKETS - 2; i++) {
		if (us > 1) {
			us >>= 1;
			bucket++;
		}
	}

	return bucket;
}

static __always_inline void hist_record(__u32 kind, __u64 ns)
{
	struct rvv_hist *hist;
	__u32 bucket;

	hist = bpf_map_lookup_elem(&hists, &kind);
	if (!hist)
		return;

	bucket = hist_bucket_us(ns);
	if (bucket >= RVV_HIST_BUCKETS)
		bucket = RVV_HIST_BUCKETS - 1;

	inc_u64(&hist->buckets[bucket]);
}

static __always_inline void futex_addr_hist_record(__u64 uaddr, __u64 ns)
{
	struct rvv_futex_addr_hist_key key = {};
	__u64 zero = 0;
	__u64 *count;

	key.uaddr = uaddr;
	key.bucket = hist_bucket_us(ns);
	if (key.bucket >= RVV_HIST_BUCKETS)
		key.bucket = RVV_HIST_BUCKETS - 1;

	count = bpf_map_lookup_elem(&futex_addr_hists, &key);
	if (!count) {
		bpf_map_update_elem(&futex_addr_hists, &key, &zero, BPF_NOEXIST);
		count = bpf_map_lookup_elem(&futex_addr_hists, &key);
	}
	if (count)
		inc_u64(count);
}

static __always_inline __u32 vs_bucket(unsigned long vs)
{
	switch (vs & RVV_SR_VS) {
	case RVV_SR_VS_OFF:
		return RVV_VS_OFF_BUCKET;
	case RVV_SR_VS_INITIAL:
		return RVV_VS_INITIAL_BUCKET;
	case RVV_SR_VS_CLEAN:
		return RVV_VS_CLEAN_BUCKET;
	case RVV_SR_VS_DIRTY:
		return RVV_VS_DIRTY_BUCKET;
	default:
		return RVV_VS_UNKNOWN_BUCKET;
	}
}

static __always_inline __u32 out_state_bucket(bool preempt,
					      unsigned long prev_state)
{
	if (preempt)
		return RVV_OUT_PREEMPT_BUCKET;
	if (prev_state == RVV_TASK_RUNNING)
		return RVV_OUT_RUNNING_BUCKET;
	if (prev_state & RVV_TASK_UNINTERRUPTIBLE)
		return RVV_OUT_UNINTERRUPTIBLE_BUCKET;
	if (prev_state & RVV_TASK_INTERRUPTIBLE)
		return RVV_OUT_SLEEPING_BUCKET;
	return RVV_OUT_OTHER_BUCKET;
}

static __always_inline void inc_out_state(__u64 buckets[RVV_OUT_STATE_BUCKETS],
					  __u32 bucket)
{
	if (bucket >= RVV_OUT_STATE_BUCKETS)
		return;
	inc_u64(&buckets[bucket]);
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

static __always_inline bool task_matches(struct task_struct *task,
					 const struct rvv_config *cfg)
{
	__u32 pid = BPF_CORE_READ(task, pid);
	__u32 tgid = BPF_CORE_READ(task, tgid);

	if (cfg->target_tid && pid != cfg->target_tid)
		return false;
	if (cfg->target_tgid && tgid != cfg->target_tgid)
		return false;

	return cfg->target_tid || cfg->target_tgid;
}

static __always_inline bool current_matches(const struct rvv_config *cfg,
					    struct rvv_task_key *key)
{
	__u64 pid_tgid = bpf_get_current_pid_tgid();

	key->tid = (__u32)pid_tgid;
	key->tgid = (__u32)(pid_tgid >> 32);
	if (cfg->target_tid && key->tid != cfg->target_tid)
		return false;
	if (cfg->target_tgid && key->tgid != cfg->target_tgid)
		return false;

	return cfg->target_tid || cfg->target_tgid;
}

static __always_inline void make_key(struct task_struct *task,
				     struct rvv_task_key *key)
{
	key->tid = BPF_CORE_READ(task, pid);
	key->tgid = BPF_CORE_READ(task, tgid);
}

static __always_inline struct rvv_thread_stats *
get_thread_stats(struct task_struct *task, const struct rvv_task_key *key)
{
	struct rvv_thread_stats *zero;
	struct rvv_thread_stats *stats;
	__u32 scratch_key = 0;

	stats = bpf_map_lookup_elem(&thread_stats, key);
	if (stats)
		return stats;

	zero = bpf_map_lookup_elem(&scratch_thread_stats, &scratch_key);
	if (!zero)
		return NULL;

	__builtin_memset(zero, 0, sizeof(*zero));
	zero->tgid = key->tgid;
	zero->tid = key->tid;
	bpf_core_read_str(zero->comm, sizeof(zero->comm), &task->comm);
	bpf_map_update_elem(&thread_stats, key, zero, BPF_NOEXIST);

	return bpf_map_lookup_elem(&thread_stats, key);
}

static __always_inline struct rvv_task_ts *
get_task_ts(const struct rvv_task_key *key)
{
	struct rvv_task_ts zero = {};
	struct rvv_task_ts *ts;

	ts = bpf_map_lookup_elem(&task_ts, key);
	if (ts)
		return ts;

	bpf_map_update_elem(&task_ts, key, &zero, BPF_NOEXIST);
	return bpf_map_lookup_elem(&task_ts, key);
}

static __always_inline void record_waker(struct task_struct *wakee)
{
	struct rvv_waker_key key = {};
	struct rvv_waker_stats zero = {};
	struct rvv_waker_stats *stats;
	__u64 pid_tgid;
	unsigned long vs;
	bool ok = false;

	key.wakee_tid = BPF_CORE_READ(wakee, pid);
	key.wakee_tgid = BPF_CORE_READ(wakee, tgid);
	pid_tgid = bpf_get_current_pid_tgid();
	key.waker_tid = (__u32)pid_tgid;
	key.waker_tgid = (__u32)(pid_tgid >> 32);

	stats = bpf_map_lookup_elem(&waker_stats, &key);
	if (!stats) {
		zero.wakee_tid = key.wakee_tid;
		zero.wakee_tgid = key.wakee_tgid;
		zero.waker_tid = key.waker_tid;
		zero.waker_tgid = key.waker_tgid;
		bpf_core_read_str(zero.wakee_comm, sizeof(zero.wakee_comm),
				  &wakee->comm);
		bpf_get_current_comm(zero.waker_comm,
				     sizeof(zero.waker_comm));
		bpf_map_update_elem(&waker_stats, &key, &zero, BPF_NOEXIST);
		stats = bpf_map_lookup_elem(&waker_stats, &key);
		if (!stats)
			return;
	}

	inc_u64(&stats->wakeups);
	vs = read_vs_state(wakee, &ok);
	if (ok && vs == RVV_SR_VS_DIRTY)
		inc_u64(&stats->wakee_dirty_wakeups);
}

static __always_inline __u32 futex_kind(long id, unsigned long op_arg)
{
	__u32 cmd;

	if (id == RVV_NR_FUTEX_WAITV)
		return 6;

	cmd = (__u32)op_arg & RVV_FUTEX_CMD_MASK;
	switch (cmd) {
	case RVV_FUTEX_WAIT:
	case RVV_FUTEX_WAIT_BITSET:
	case RVV_FUTEX_WAIT_REQUEUE_PI:
		return 1;
	case RVV_FUTEX_WAKE:
	case RVV_FUTEX_WAKE_BITSET:
		return 2;
	case RVV_FUTEX_REQUEUE:
	case RVV_FUTEX_CMP_REQUEUE:
	case RVV_FUTEX_CMP_REQUEUE_PI:
		return 3;
	case RVV_FUTEX_WAKE_OP:
		return 4;
	case RVV_FUTEX_LOCK_PI:
	case RVV_FUTEX_UNLOCK_PI:
	case RVV_FUTEX_TRYLOCK_PI:
	case RVV_FUTEX_LOCK_PI2:
		return 5;
	default:
		return 7;
	}
}

static __always_inline __u32 futex_wait_ret_bucket(long ret)
{
	if (!ret)
		return RVV_FUTEX_RET_WOKEN;
	if (ret == -RVV_EAGAIN)
		return RVV_FUTEX_RET_EAGAIN;
	if (ret == -RVV_EINTR)
		return RVV_FUTEX_RET_EINTR;
	if (ret == -RVV_ETIMEDOUT)
		return RVV_FUTEX_RET_ETIMEDOUT;
	return RVV_FUTEX_RET_OTHER;
}

static __always_inline bool futex_wait_was_blocked(long ret)
{
	return !ret || ret == -RVV_EINTR || ret == -RVV_ETIMEDOUT ||
	       ret == -RVV_ERESTARTSYS;
}

static __always_inline void record_futex_wait_duration(
	__u64 *completed_ops, __u64 *completed_total_ns,
	__u64 *blocked_ops, __u64 *blocked_total_ns, __u64 *blocked_max_ns,
	__u64 ret_counts[RVV_FUTEX_RET_BUCKETS],
	__u64 ret_total_ns[RVV_FUTEX_RET_BUCKETS], __u64 delta, long ret)
{
	__u32 bucket = futex_wait_ret_bucket(ret);

	inc_u64(completed_ops);
	add_u64(completed_total_ns, delta);
	if (bucket < RVV_FUTEX_RET_BUCKETS) {
		inc_u64(&ret_counts[bucket]);
		add_u64(&ret_total_ns[bucket], delta);
	}
	if (!futex_wait_was_blocked(ret))
		return;

	inc_u64(blocked_ops);
	add_u64(blocked_total_ns, delta);
	update_max(blocked_max_ns, delta);
}

static __always_inline void inc_futex_counters(__u64 *syscalls,
					       __u64 *wait_ops,
					       __u64 *wake_ops,
					       __u64 *requeue_ops,
					       __u64 *wake_op_ops,
					       __u64 *pi_ops,
					       __u64 *waitv_syscalls,
					       __u64 *other_ops,
					       __u32 kind)
{
	inc_u64(syscalls);
	switch (kind) {
	case 1:
		inc_u64(wait_ops);
		break;
	case 2:
		inc_u64(wake_ops);
		break;
	case 3:
		inc_u64(requeue_ops);
		break;
	case 4:
		inc_u64(wake_op_ops);
		break;
	case 5:
		inc_u64(pi_ops);
		break;
	case 6:
		inc_u64(waitv_syscalls);
		break;
	default:
		inc_u64(other_ops);
		break;
	}
}

static __always_inline void record_futex_addr(struct task_struct *task,
					      const struct rvv_task_key *task_key,
					      unsigned long uaddr,
					      __u32 kind,
					      bool detail)
{
	struct rvv_futex_addr_key addr_key = {};
	struct rvv_futex_tid_key tid_key = {};
	struct rvv_futex_addr_stats *addr_zero;
	struct rvv_futex_tid_stats *tid_zero;
	struct rvv_futex_addr_stats *addr_stats;
	struct rvv_futex_tid_stats *tid_stats;
	__u32 scratch_key = 0;

	if (!uaddr)
		return;

	addr_key.uaddr = uaddr;
	addr_stats = bpf_map_lookup_elem(&futex_addr_stats, &addr_key);
	if (!addr_stats) {
		addr_zero = bpf_map_lookup_elem(&scratch_futex_addr_stats,
						&scratch_key);
		if (!addr_zero)
			return;
		__builtin_memset(addr_zero, 0, sizeof(*addr_zero));
		addr_zero->uaddr = uaddr;
		addr_zero->last_tgid = task_key->tgid;
		addr_zero->last_tid = task_key->tid;
		bpf_core_read_str(addr_zero->last_comm, sizeof(addr_zero->last_comm),
				  &task->comm);
		bpf_map_update_elem(&futex_addr_stats, &addr_key, addr_zero,
				    BPF_NOEXIST);
		addr_stats = bpf_map_lookup_elem(&futex_addr_stats, &addr_key);
	}
	if (addr_stats) {
		inc_futex_counters(&addr_stats->syscalls,
				   &addr_stats->wait_ops,
				   &addr_stats->wake_ops,
				   &addr_stats->requeue_ops,
				   &addr_stats->wake_op_ops,
				   &addr_stats->pi_ops,
				   &addr_stats->waitv_syscalls,
				   &addr_stats->other_ops, kind);
	}

	if (!detail)
		return;

	tid_key.uaddr = uaddr;
	tid_key.tgid = task_key->tgid;
	tid_key.tid = task_key->tid;
	tid_stats = bpf_map_lookup_elem(&futex_tid_stats, &tid_key);
	if (!tid_stats) {
		tid_zero = bpf_map_lookup_elem(&scratch_futex_tid_stats,
					       &scratch_key);
		if (!tid_zero)
			return;
		__builtin_memset(tid_zero, 0, sizeof(*tid_zero));
		tid_zero->uaddr = uaddr;
		tid_zero->tgid = task_key->tgid;
		tid_zero->tid = task_key->tid;
		bpf_core_read_str(tid_zero->comm, sizeof(tid_zero->comm),
				  &task->comm);
		bpf_map_update_elem(&futex_tid_stats, &tid_key, tid_zero,
				    BPF_NOEXIST);
		tid_stats = bpf_map_lookup_elem(&futex_tid_stats, &tid_key);
	}
	if (tid_stats)
		inc_futex_counters(&tid_stats->syscalls,
				   &tid_stats->wait_ops,
				   &tid_stats->wake_ops,
				   &tid_stats->requeue_ops,
				   &tid_stats->wake_op_ops,
				   &tid_stats->pi_ops,
				   &tid_stats->waitv_syscalls,
				   &tid_stats->other_ops, kind);
}

static __always_inline void record_futex_stack(struct task_struct *task,
					       const struct rvv_task_key *task_key,
					       unsigned long uaddr,
					       __u32 kind,
					       int stack_id)
{
	struct rvv_futex_stack_key stack_key = {};
	struct rvv_futex_stack_stats zero = {};
	struct rvv_futex_stack_stats *stats;

	if (!uaddr || stack_id < 0)
		return;

	stack_key.uaddr = uaddr;
	stack_key.stack_id = stack_id;
	stack_key.kind = kind;
	stats = bpf_map_lookup_elem(&futex_stack_stats, &stack_key);
	if (!stats) {
		zero.uaddr = uaddr;
		zero.stack_id = stack_id;
		zero.kind = kind;
		zero.last_tgid = task_key->tgid;
		zero.last_tid = task_key->tid;
		bpf_core_read_str(zero.last_comm, sizeof(zero.last_comm),
				  &task->comm);
		bpf_map_update_elem(&futex_stack_stats, &stack_key, &zero,
				    BPF_NOEXIST);
		stats = bpf_map_lookup_elem(&futex_stack_stats, &stack_key);
	}
	if (!stats)
		return;

	inc_futex_counters(&stats->syscalls, &stats->wait_ops,
			   &stats->wake_ops, &stats->requeue_ops,
			   &stats->wake_op_ops, &stats->pi_ops,
			   &stats->waitv_syscalls, &stats->other_ops, kind);
}

static __always_inline void record_futex_syscall(struct task_struct *task,
						 const struct rvv_task_key *task_key,
						 struct rvv_global_stats *g,
						 struct rvv_thread_stats *t,
						 void *ctx,
						 long id,
						 unsigned long uaddr,
						 unsigned long op_arg,
						 bool detail,
						 bool stack)
{
	__u32 kind = futex_kind(id, op_arg);

	inc_futex_counters(&g->futex_syscalls, &g->futex_wait_ops,
			   &g->futex_wake_ops, &g->futex_requeue_ops,
			   &g->futex_wake_op_ops, &g->futex_pi_ops,
			   &g->futex_waitv_syscalls, &g->futex_other_ops, kind);
	inc_futex_counters(&t->futex_syscalls, &t->futex_wait_ops,
			   &t->futex_wake_ops, &t->futex_requeue_ops,
			   &t->futex_wake_op_ops, &t->futex_pi_ops,
			   &t->futex_waitv_syscalls, &t->futex_other_ops, kind);

	if (id != RVV_NR_FUTEX_WAITV) {
		record_futex_addr(task, task_key, uaddr, kind, detail);
		if (stack) {
			int stack_id;

			stack_id = bpf_get_stackid(ctx, &futex_user_stacks,
						   BPF_F_USER_STACK);
			record_futex_stack(task, task_key, uaddr, kind, stack_id);
		}
	}
}

static __always_inline void record_switch_interval(struct rvv_global_stats *g,
						   struct rvv_thread_stats *t,
						   struct rvv_task_ts *ts,
						   __u64 now)
{
	__u64 delta;

	if (!ts->last_switch_ts) {
		ts->last_switch_ts = now;
		return;
	}

	delta = now - ts->last_switch_ts;
	ts->last_switch_ts = now;
	inc_u64(&g->switch_interval_events);
	add_u64(&g->switch_interval_total_ns, delta);
	update_max(&g->switch_interval_max_ns, delta);
	inc_u64(&t->switch_interval_events);
	add_u64(&t->switch_interval_total_ns, delta);
	update_max(&t->switch_interval_max_ns, delta);
	hist_record(RVV_HIST_SWITCH_INTERVAL, delta);
}

static __always_inline void record_prev(struct task_struct *prev,
					struct rvv_global_stats *g,
					struct rvv_cpu_stats *c,
					__u64 now,
					const struct rvv_config *cfg,
					bool preempt,
					unsigned long prev_state)
{
	struct rvv_thread_stats *t;
	struct rvv_task_key key = {};
	struct rvv_task_ts *ts;
	unsigned long vs = 0;
	bool ok = false;
	__u32 bucket;
	__u32 out_bucket = RVV_OUT_OTHER_BUCKET;

	make_key(prev, &key);
	t = get_thread_stats(prev, &key);
	ts = get_task_ts(&key);
	if (!t || !ts) {
		inc_u64(&g->lost_ts_updates);
		return;
	}

	inc_u64(&g->switches_out);
	inc_u64(&t->switches_out);
	inc_u64(&c->switches_out);
	if (cfg->sample_out_state) {
		out_bucket = out_state_bucket(preempt, prev_state);
		inc_out_state(g->switches_out_state, out_bucket);
		inc_out_state(t->switches_out_state, out_bucket);
		inc_out_state(c->switches_out_state, out_bucket);
	}

	ts->offcpu_ts = now;
	record_switch_interval(g, t, ts, now);

	if (!cfg->sample_vs)
		return;

	vs = read_vs_state(prev, &ok);
	bucket = ok ? vs_bucket(vs) : RVV_VS_UNKNOWN_BUCKET;
	inc_u64(&g->prev_vs_samples[bucket]);
	inc_u64(&t->prev_vs_samples[bucket]);

	if (ok && vs == RVV_SR_VS_DIRTY) {
		inc_u64(&g->v_save_candidates);
		inc_u64(&t->v_save_candidates);
		inc_u64(&c->v_save_candidates);
		if (cfg->sample_out_state) {
			inc_out_state(g->v_save_out_state, out_bucket);
			inc_out_state(t->v_save_out_state, out_bucket);
			inc_out_state(c->v_save_out_state, out_bucket);
		}
	}
}

static __always_inline void record_next(struct task_struct *next,
					struct rvv_global_stats *g,
					struct rvv_cpu_stats *c,
					__u64 now,
					const struct rvv_config *cfg)
{
	struct rvv_thread_stats *t;
	struct rvv_task_key key = {};
	struct rvv_task_ts *ts;
	unsigned long vs = 0;
	__u64 delta;
	bool ok = false;
	__u32 bucket;

	make_key(next, &key);
	t = get_thread_stats(next, &key);
	ts = get_task_ts(&key);
	if (!t || !ts) {
		inc_u64(&g->lost_ts_updates);
		return;
	}

	inc_u64(&g->switches_in);
	inc_u64(&t->switches_in);
	inc_u64(&c->switches_in);

	if (ts->offcpu_ts) {
		delta = now - ts->offcpu_ts;
		ts->offcpu_ts = 0;
		inc_u64(&g->offcpu_events);
		add_u64(&g->offcpu_total_ns, delta);
		update_max(&g->offcpu_max_ns, delta);
		inc_u64(&t->offcpu_events);
		add_u64(&t->offcpu_total_ns, delta);
		update_max(&t->offcpu_max_ns, delta);
		hist_record(RVV_HIST_OFFCPU, delta);
	}

	if (ts->wake_ts) {
		delta = now - ts->wake_ts;
		ts->wake_ts = 0;
		inc_u64(&g->wake_to_run_events);
		add_u64(&g->wake_to_run_total_ns, delta);
		update_max(&g->wake_to_run_max_ns, delta);
		inc_u64(&t->wake_to_run_events);
		add_u64(&t->wake_to_run_total_ns, delta);
		update_max(&t->wake_to_run_max_ns, delta);
		hist_record(RVV_HIST_WAKE_TO_RUN, delta);
	}

	record_switch_interval(g, t, ts, now);

	if (!cfg->sample_vs)
		return;

	vs = read_vs_state(next, &ok);
	bucket = ok ? vs_bucket(vs) : RVV_VS_UNKNOWN_BUCKET;
	inc_u64(&g->next_vs_samples[bucket]);
	inc_u64(&t->next_vs_samples[bucket]);

	if (ok && vs != RVV_SR_VS_OFF) {
		inc_u64(&g->v_restore_candidates);
		inc_u64(&t->v_restore_candidates);
		inc_u64(&c->v_restore_candidates);
	}
}

static __always_inline void record_rvv_only(struct task_struct *task,
					     struct rvv_global_stats *g,
					     bool switching_out)
{
	unsigned long vs;
	bool ok = false;

	vs = read_vs_state(task, &ok);
	if (!ok)
		return;

	if (switching_out) {
		if (vs == RVV_SR_VS_DIRTY)
			inc_u64(&g->v_save_candidates);
	} else if (vs != RVV_SR_VS_OFF) {
		inc_u64(&g->v_restore_candidates);
	}
}

SEC("tp_btf/sched_switch")
int BPF_PROG(on_sched_switch, bool preempt, struct task_struct *prev,
	     struct task_struct *next, unsigned int prev_state)
{
	struct rvv_global_stats *g;
	struct rvv_cpu_stats *c;
	struct rvv_config *cfg;
	bool prev_hit, next_hit, monitor_prev, monitor_next;
	__u32 zero = 0;
	__u32 cpu;
	__u64 now;

	cfg = bpf_map_lookup_elem(&cfg_map, &zero);
	if (!cfg)
		return 0;

	prev_hit = task_matches(prev, cfg);
	next_hit = task_matches(next, cfg);
	monitor_prev = prev_hit && cfg->sample_switch_out;
	monitor_next = next_hit;
	if (!monitor_prev && !monitor_next)
		return 0;

	g = bpf_map_lookup_elem(&global_stats, &zero);
	if (!g)
		return 0;
	if (cfg->rvv_only) {
		if (monitor_prev)
			record_rvv_only(prev, g, true);
		if (monitor_next)
			record_rvv_only(next, g, false);
		return 0;
	}

	cpu = bpf_get_smp_processor_id();
	c = bpf_map_lookup_elem(&cpu_stats, &cpu);
	if (!c)
		return 0;

	c->cpu = cpu;
	now = bpf_ktime_get_ns();
	inc_u64(&g->switch_events);
	inc_u64(&c->switch_events);

	if (monitor_prev)
		record_prev(prev, g, c, now, cfg, preempt, prev_state);
	if (monitor_next)
		record_next(next, g, c, now, cfg);

	return 0;
}

SEC("tp_btf/sched_wakeup")
int BPF_PROG(on_sched_wakeup, struct task_struct *p)
{
	struct rvv_global_stats *g;
	struct rvv_thread_stats *t;
	struct rvv_cpu_stats *c;
	struct rvv_task_key key = {};
	struct rvv_task_ts *ts;
	struct rvv_config *cfg;
	__u32 zero = 0;
	__u32 cpu;
	__u64 now;

	cfg = bpf_map_lookup_elem(&cfg_map, &zero);
	if (!cfg || !task_matches(p, cfg))
		return 0;

	g = bpf_map_lookup_elem(&global_stats, &zero);
	if (!g)
		return 0;

	make_key(p, &key);
	t = get_thread_stats(p, &key);
	ts = get_task_ts(&key);
	if (!t || !ts) {
		inc_u64(&g->lost_ts_updates);
		return 0;
	}

	cpu = bpf_get_smp_processor_id();
	c = bpf_map_lookup_elem(&cpu_stats, &cpu);
	if (!c)
		return 0;

	c->cpu = cpu;
	now = bpf_ktime_get_ns();
	ts->wake_ts = now;
	record_waker(p);

	inc_u64(&g->wakeups);
	inc_u64(&t->wakeups);
	inc_u64(&c->wakeups);

	return 0;
}

SEC("tracepoint/raw_syscalls/sys_enter")
int on_sys_enter(struct trace_event_raw_sys_enter *ctx)
{
	struct rvv_global_stats *g;
	struct rvv_thread_stats *t;
	struct rvv_task_key key = {};
	struct rvv_config *cfg;
	struct task_struct *task;
	__u32 zero = 0;
	long id;

	cfg = bpf_map_lookup_elem(&cfg_map, &zero);
	if (!cfg || !cfg->sample_futex)
		return 0;

	id = ctx->id;
	if (id != RVV_NR_FUTEX && id != RVV_NR_FUTEX_TIME64 &&
	    id != RVV_NR_FUTEX_WAITV)
		return 0;

	if (!current_matches(cfg, &key))
		return 0;

	g = bpf_map_lookup_elem(&global_stats, &zero);
	if (!g)
		return 0;

	task = (struct task_struct *)bpf_get_current_task_btf();
	t = get_thread_stats(task, &key);
	if (!t) {
		inc_u64(&g->lost_ts_updates);
		return 0;
	}

	record_futex_syscall(task, &key, g, t, ctx, id, ctx->args[0],
			     ctx->args[1], cfg->sample_futex_detail,
			     cfg->sample_futex_stack);

	if (cfg->sample_futex_time && id != RVV_NR_FUTEX_WAITV &&
	    futex_kind(id, ctx->args[1]) == 1) {
		struct rvv_futex_wait_inflight inflight = {
			.start_ns = bpf_ktime_get_ns(),
			.uaddr = ctx->args[0],
		};

		if (bpf_map_update_elem(&futex_wait_inflight, &key, &inflight,
					BPF_ANY))
			inc_u64(&g->futex_wait_start_failures);
	}
	return 0;
}

SEC("tracepoint/raw_syscalls/sys_exit")
int on_sys_exit(struct trace_event_raw_sys_exit *ctx)
{
	struct rvv_futex_wait_inflight *inflight_ptr;
	struct rvv_futex_wait_inflight inflight;
	struct rvv_futex_addr_key addr_key = {};
	struct rvv_futex_tid_key tid_key = {};
	struct rvv_futex_addr_stats *addr_stats;
	struct rvv_futex_tid_stats *tid_stats;
	struct rvv_global_stats *g;
	struct rvv_thread_stats *t;
	struct rvv_task_key key = {};
	struct rvv_config *cfg;
	struct task_struct *task;
	__u32 zero = 0;
	__u64 now;
	__u64 delta;
	long id;
	long ret;

	cfg = bpf_map_lookup_elem(&cfg_map, &zero);
	if (!cfg || !cfg->sample_futex_time)
		return 0;

	id = ctx->id;
	if (id != RVV_NR_FUTEX && id != RVV_NR_FUTEX_TIME64)
		return 0;
	if (!current_matches(cfg, &key))
		return 0;

	inflight_ptr = bpf_map_lookup_elem(&futex_wait_inflight, &key);
	if (!inflight_ptr)
		return 0;
	inflight = *inflight_ptr;
	bpf_map_delete_elem(&futex_wait_inflight, &key);

	now = bpf_ktime_get_ns();
	if (now < inflight.start_ns)
		return 0;
	delta = now - inflight.start_ns;
	ret = ctx->ret;

	g = bpf_map_lookup_elem(&global_stats, &zero);
	if (!g)
		return 0;
	task = (struct task_struct *)bpf_get_current_task_btf();
	t = get_thread_stats(task, &key);
	if (!t) {
		inc_u64(&g->lost_ts_updates);
		return 0;
	}

	record_futex_wait_duration(
		&g->futex_wait_completed_ops, &g->futex_wait_completed_total_ns,
		&g->futex_wait_blocked_ops, &g->futex_wait_blocked_total_ns,
		&g->futex_wait_blocked_max_ns, g->futex_wait_ret,
		g->futex_wait_ret_total_ns, delta, ret);
	record_futex_wait_duration(
		&t->futex_wait_completed_ops, &t->futex_wait_completed_total_ns,
		&t->futex_wait_blocked_ops, &t->futex_wait_blocked_total_ns,
		&t->futex_wait_blocked_max_ns, t->futex_wait_ret,
		t->futex_wait_ret_total_ns, delta, ret);

	addr_key.uaddr = inflight.uaddr;
	addr_stats = bpf_map_lookup_elem(&futex_addr_stats, &addr_key);
	if (addr_stats)
		record_futex_wait_duration(
			&addr_stats->wait_completed_ops,
			&addr_stats->wait_completed_total_ns,
			&addr_stats->wait_blocked_ops,
			&addr_stats->wait_blocked_total_ns,
			&addr_stats->wait_blocked_max_ns, addr_stats->wait_ret,
			addr_stats->wait_ret_total_ns, delta, ret);

	if (cfg->sample_futex_detail) {
		tid_key.uaddr = inflight.uaddr;
		tid_key.tgid = key.tgid;
		tid_key.tid = key.tid;
		tid_stats = bpf_map_lookup_elem(&futex_tid_stats, &tid_key);
		if (tid_stats)
			record_futex_wait_duration(
				&tid_stats->wait_completed_ops,
				&tid_stats->wait_completed_total_ns,
				&tid_stats->wait_blocked_ops,
				&tid_stats->wait_blocked_total_ns,
				&tid_stats->wait_blocked_max_ns,
				tid_stats->wait_ret,
				tid_stats->wait_ret_total_ns, delta, ret);
	}

	if (futex_wait_was_blocked(ret)) {
		hist_record(RVV_HIST_FUTEX_WAIT_BLOCKED, delta);
		futex_addr_hist_record(inflight.uaddr, delta);
	}
	return 0;
}
