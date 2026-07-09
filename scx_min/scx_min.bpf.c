// SPDX-License-Identifier: GPL-2.0
#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>

char LICENSE[] SEC("license") = "GPL";

void scx_bpf_dispatch(struct task_struct *p, u64 dsq_id, u64 slice,
		      u64 enq_flags) __ksym;
bool scx_bpf_consume(u64 dsq_id) __ksym;
void scx_bpf_switch_all(void) __ksym;

SEC("struct_ops/scx_min_init")
s32 BPF_PROG(scx_min_init)
{
	scx_bpf_switch_all();
	return 0;
}

SEC("struct_ops/scx_min_enqueue")
void BPF_PROG(scx_min_enqueue, struct task_struct *p, u64 enq_flags)
{
	scx_bpf_dispatch(p, SCX_DSQ_GLOBAL, SCX_SLICE_DFL, enq_flags);
}

SEC("struct_ops/scx_min_dispatch")
void BPF_PROG(scx_min_dispatch, s32 cpu, struct task_struct *prev)
{
	scx_bpf_consume(SCX_DSQ_GLOBAL);
}

SEC("struct_ops/scx_min_exit")
void BPF_PROG(scx_min_exit, struct scx_exit_info *ei)
{
}

SEC(".struct_ops")
struct sched_ext_ops scx_min_ops = {
	.init			= (void *)scx_min_init,
	.enqueue		= (void *)scx_min_enqueue,
	.dispatch		= (void *)scx_min_dispatch,
	.exit			= (void *)scx_min_exit,
	.dispatch_max_batch	= 1,
	.timeout_ms		= 5000,
	.name			= "scx_min",
};
