// SPDX-License-Identifier: GPL-2.0
#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>

char LICENSE[] SEC("license") = "GPL";

SEC("struct_ops/scx_nop_init")
s32 BPF_PROG(scx_nop_init)
{
	return 0;
}

SEC("struct_ops/scx_nop_exit")
void BPF_PROG(scx_nop_exit, struct scx_exit_info *ei)
{
}

SEC(".struct_ops")
struct sched_ext_ops scx_nop_ops = {
	.init		= (void *)scx_nop_init,
	.exit		= (void *)scx_nop_exit,
	.timeout_ms	= 3000,
	.name		= "scx_nop",
};
