// SPDX-License-Identifier: GPL-2.0
#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
#include "control.h"

char LICENSE[] SEC("license") = "GPL";
/* Linux 6.18 把 dispatch/consume 改名为 dsq_insert/dsq_move_to_local，
 * 且 global DSQ 由内核自动消费、switch_all 不再需要。用编译宏切换。 */
#ifdef SCX_API_DSQ_INSERT
void scx_bpf_dsq_insert(struct task_struct *, __u64, __u64, __u64) __ksym;
bool scx_bpf_dsq_move_to_local(__u64) __ksym;
#define scx_bpf_dispatch scx_bpf_dsq_insert
#define scx_bpf_consume scx_bpf_dsq_move_to_local
#else
void scx_bpf_dispatch(struct task_struct *, __u64, __u64, __u64) __ksym;
void scx_bpf_switch_all(void) __ksym;
bool scx_bpf_consume(__u64) __ksym;
#endif
int scx_bpf_create_dsq(__u64, int) __ksym;
void scx_bpf_kick_cpu(__s32, __u64) __ksym;
bool bpf_cpumask_test_cpu(__u32, const struct cpumask *) __ksym;

#define DT_DSQ_BASE 0x44540000ULL
/* 每 (cpu, team) 一条自定义 DSQ，便于按 team 优先级消费（x86 phase lease）。 */
#define DT_DSQ(cpu, team) (DT_DSQ_BASE + (__u64)(cpu) * DT_TEAMS + (__u64)(team))

struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, DT_TEAMS);
    __type(key, __u32);
    __type(value, struct dt_bpf_grant);
} grants SEC(".maps");
struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, DT_TEAMS * DT_WORKERS);
    __type(key, __u32);
    __type(value, struct dt_bpf_member);
} members SEC(".maps");
/* sched_ext enable/disable callbacks give userspace an exact task generation.
 * This closes the window where a dead worker's numeric TID is reused before
 * the next graph boundary removes its persistent member entry. */
struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 32768);
    __type(key, __u32);
    __type(value, __u64);
} identities SEC(".maps");
/* 处于 prefill 的 team 位图；dispatch 时优先消费这些 team 的 DSQ。 */
struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 1);
    __type(key, __u32);
    __type(value, __u64);
} prefill_mask SEC(".maps");

__u64 enqueue_count, placed_count, running_count, wrong_cpu_count, stale_identity_count;
__u32 exit_kind;
char exit_reason[128];

static __always_inline int target_cpu(struct task_struct *p)
{
    __u32 tid = p->pid;
    struct dt_bpf_member *m = bpf_map_lookup_elem(&members, &tid);
    if (!m || m->slot >= DT_TEAMS || m->cpu >= DT_WORKERS)
        return -1;
    __u32 slot = m->slot;
    struct dt_bpf_grant *g = bpf_map_lookup_elem(&grants, &slot);
    if (!g || !g->ready || !g->pin || g->tgid != p->tgid ||
        g->session != m->session || g->epoch != m->epoch ||
        !(g->cpu_mask & (1ULL << m->cpu)) ||
        !bpf_cpumask_test_cpu(m->cpu, p->cpus_ptr))
        return -1;
    if (m->start_boottime != p->start_boottime) {
        __sync_fetch_and_add(&stale_identity_count, 1);
        return -1;
    }
    return m->cpu;
}

/* x86 / Linux 6.18: enable(struct task_struct *) 无 scx_enable_args 参数。 */
SEC("struct_ops/dtc_enable")
void BPF_PROG(dtc_enable, struct task_struct *p)
{
    __u32 tid = p->pid;
    __u64 start = p->start_boottime;
    bpf_map_update_elem(&identities, &tid, &start, BPF_ANY);
}

SEC("struct_ops/dtc_disable")
void BPF_PROG(dtc_disable, struct task_struct *p)
{
    __u32 tid = p->pid;
    bpf_map_delete_elem(&identities, &tid);
}

SEC("struct_ops/dtc_select_cpu")
int BPF_PROG(dtc_select_cpu, struct task_struct *p, int prev_cpu, __u64 wake_flags)
{
    int cpu = target_cpu(p);
    return cpu >= 0 ? cpu : prev_cpu;
}

SEC("struct_ops/dtc_enqueue")
void BPF_PROG(dtc_enqueue, struct task_struct *p, __u64 flags)
{
    __sync_fetch_and_add(&enqueue_count, 1);
    int cpu = target_cpu(p);
    if (cpu >= 0) {
        __u32 tid = p->pid;
        struct dt_bpf_member *m = bpf_map_lookup_elem(&members, &tid);
        __u32 slot = m ? m->slot : 0;
        __sync_fetch_and_add(&placed_count, 1);
        /* LOCAL_ON is illegal for direct dispatch from enqueue on this ABI. */
        scx_bpf_dispatch(p, DT_DSQ(cpu, slot), SCX_SLICE_DFL, flags);
        /* prefill worker 唤醒时定向抢占目标核上的 decode worker。 */
        __u32 zero = 0;
        __u64 *pm = bpf_map_lookup_elem(&prefill_mask, &zero);
        if (pm && (*pm & (1ULL << slot)) && (flags & SCX_ENQ_WAKEUP))
            scx_bpf_kick_cpu(cpu, SCX_KICK_PREEMPT);
    } else {
        scx_bpf_dispatch(p, SCX_DSQ_GLOBAL, SCX_SLICE_DFL, flags);
    }
}

SEC("struct_ops/dtc_dispatch")
void BPF_PROG(dtc_dispatch, int cpu, struct task_struct *prev)
{
    if (cpu < 0 || cpu >= DT_WORKERS)
        return;
    __u32 zero = 0;
    __u64 *pm = bpf_map_lookup_elem(&prefill_mask, &zero);
    __u64 mask = pm ? *pm : 0;
    /* prefill team 优先；其队列为空时再服务其他 team。 */
#pragma unroll
    for (unsigned t = 0; t < DT_TEAMS; t++)
        if ((mask & (1ULL << t)) && scx_bpf_consume(DT_DSQ(cpu, t)))
            return;
#pragma unroll
    for (unsigned t = 0; t < DT_TEAMS; t++)
        if (!(mask & (1ULL << t)) && scx_bpf_consume(DT_DSQ(cpu, t)))
            return;
}

SEC("struct_ops/dtc_running")
void BPF_PROG(dtc_running, struct task_struct *p)
{
    int cpu = target_cpu(p);
    if (cpu >= 0) {
        __sync_fetch_and_add(&running_count, 1);
        if (cpu != bpf_get_smp_processor_id())
            __sync_fetch_and_add(&wrong_cpu_count, 1);
    }
}

SEC("struct_ops/dtc_init")
int BPF_PROG(dtc_init)
{
    for (unsigned cpu = 0; cpu < DT_WORKERS; cpu++) {
        for (unsigned team = 0; team < DT_TEAMS; team++) {
            int error = scx_bpf_create_dsq(DT_DSQ(cpu, team), -1);
            if (error)
                return error;
        }
    }
#ifndef SCX_API_DSQ_INSERT
    scx_bpf_switch_all();
#endif
    return 0;
}
SEC("struct_ops/dtc_exit")
void BPF_PROG(dtc_exit, struct scx_exit_info *ei)
{
    exit_kind = ei->kind;
    bpf_probe_read_kernel_str(exit_reason, sizeof(exit_reason), ei->reason);
}
SEC(".struct_ops")
struct sched_ext_ops dtc_ops = {
    .select_cpu = (void *)dtc_select_cpu,
    .enqueue = (void *)dtc_enqueue,
    .dispatch = (void *)dtc_dispatch,
    .running = (void *)dtc_running,
    .enable = (void *)dtc_enable,
    .disable = (void *)dtc_disable,
    .init = (void *)dtc_init,
    .exit = (void *)dtc_exit,
    .name = "dt_control",
};
