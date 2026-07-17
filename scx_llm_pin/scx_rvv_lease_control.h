// SPDX-License-Identifier: GPL-2.0 OR MIT
#ifndef __SCX_RVV_LEASE_CONTROL_H
#define __SCX_RVV_LEASE_CONTROL_H

#ifndef __VMLINUX_H__
#include <linux/types.h>
#endif

#define RVV_LEASE_CONTROL_MAGIC       0x524c4354U
#define RVV_LEASE_CONTROL_ABI_VERSION 1U
#define RVV_LEASE_CONTROL_WORKERS     8U
#define RVV_LEASE_CONTROL_DEFAULT_DIR "/sys/fs/bpf/scx_rvv_dirty_lease"
#define RVV_LEASE_CONFIG_MAP_NAME     "config"
#define RVV_LEASE_WORKER_MAP_NAME     "workers"

enum rvv_lease_control_mode {
	RVV_LEASE_CONTROL_OFF = 0,
	RVV_LEASE_CONTROL_OBSERVE = 1,
	RVV_LEASE_CONTROL_ACTIVE = 2,
};

struct rvv_lease_worker_header {
	__u32 magic;
	__u32 abi_version;
	__u32 active;
	__u32 target_tgid;
	__u32 max_workers;
	__u32 loader_pid;
	__u64 session_id;
	__u64 policy_epoch;
	__u64 reserved64[3];
} __attribute__((aligned(64)));

struct rvv_lease_worker_slot {
	__u32 sequence;
	__u32 tid;
	__u32 tgid;
	__u32 ith;
	__u32 n_threads;
	__u32 flags;
	__u64 session_id;
	__u64 reserved64[4];
} __attribute__((aligned(64)));

struct rvv_lease_worker_page {
	struct rvv_lease_worker_header header;
	struct rvv_lease_worker_slot workers[RVV_LEASE_CONTROL_WORKERS];
};

#ifndef __VMLINUX_H__
_Static_assert(sizeof(struct rvv_lease_worker_header) == 64,
	       "lease worker header must occupy one cache line");
_Static_assert(sizeof(struct rvv_lease_worker_slot) == 64,
	       "lease worker slot must occupy one cache line");
_Static_assert(sizeof(struct rvv_lease_worker_page) <= 4096,
	       "lease worker page must fit in one page");
#endif

#endif /* __SCX_RVV_LEASE_CONTROL_H */
