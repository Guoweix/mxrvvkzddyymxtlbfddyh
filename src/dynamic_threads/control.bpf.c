// SPDX-License-Identifier: GPL-2.0
#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
#include "control.h"

char LICENSE[] SEC("license") = "GPL";
void scx_bpf_dispatch(struct task_struct *, __u64, __u64, __u64) __ksym;
void scx_bpf_switch_all(void) __ksym;
int scx_bpf_create_dsq(__u64, int) __ksym;
bool scx_bpf_consume(__u64) __ksym;
bool bpf_cpumask_test_cpu(__u32, const struct cpumask *) __ksym;

#define DT_DSQ_BASE 0x44540000ULL
#define DT_DSQ(cpu) (DT_DSQ_BASE + (cpu))

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

SEC("struct_ops/dtc_enable")
void BPF_PROG(dtc_enable, struct task_struct *p, struct scx_enable_args *args)
{
    __u32 tid = p->pid;
    __u64 start = p->start_boottime;
    (void)args;
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
        __sync_fetch_and_add(&placed_count, 1);
        /* LOCAL_ON is illegal for direct dispatch from enqueue on this ABI. */
        scx_bpf_dispatch(p, DT_DSQ(cpu), SCX_SLICE_DFL, flags);
    } else {
        scx_bpf_dispatch(p, SCX_DSQ_GLOBAL, SCX_SLICE_DFL, flags);
    }
}

SEC("struct_ops/dtc_dispatch")
void BPF_PROG(dtc_dispatch, int cpu, struct task_struct *prev)
{
    if (cpu >= 0 && cpu < DT_WORKERS && scx_bpf_consume(DT_DSQ(cpu)))
        return;
    scx_bpf_consume(SCX_DSQ_GLOBAL);
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
        int error = scx_bpf_create_dsq(DT_DSQ(cpu), -1);
        if (error)
            return error;
    }
    scx_bpf_switch_all();
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
