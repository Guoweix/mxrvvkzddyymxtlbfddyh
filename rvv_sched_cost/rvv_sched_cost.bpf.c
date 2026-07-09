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
					const struct rvv_config *cfg)
{
	struct rvv_thread_stats *t;
	struct rvv_task_key key = {};
	struct rvv_task_ts *ts;
	unsigned long vs = 0;
	bool ok = false;
	__u32 bucket;

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

SEC("tp_btf/sched_switch")
int BPF_PROG(on_sched_switch, bool preempt, struct task_struct *prev,
	     struct task_struct *next, unsigned int prev_state)
{
	struct rvv_global_stats *g;
	struct rvv_cpu_stats *c;
	struct rvv_config *cfg;
	bool prev_hit, next_hit;
	__u32 zero = 0;
	__u32 cpu;
	__u64 now;

	cfg = bpf_map_lookup_elem(&cfg_map, &zero);
	if (!cfg)
		return 0;

	prev_hit = task_matches(prev, cfg);
	next_hit = task_matches(next, cfg);
	if (!prev_hit && !next_hit)
		return 0;

	g = bpf_map_lookup_elem(&global_stats, &zero);
	if (!g)
		return 0;

	cpu = bpf_get_smp_processor_id();
	c = bpf_map_lookup_elem(&cpu_stats, &cpu);
	if (!c)
		return 0;

	c->cpu = cpu;
	now = bpf_ktime_get_ns();
	inc_u64(&g->switch_events);
	inc_u64(&c->switch_events);

	if (prev_hit)
		record_prev(prev, g, c, now, cfg);
	if (next_hit)
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

	inc_u64(&g->wakeups);
	inc_u64(&t->wakeups);
	inc_u64(&c->wakeups);

	return 0;
}
