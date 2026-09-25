// SPDX-License-Identifier: GPL-2.0 OR MIT
#ifndef __SCX_RVV_LEASE_CONTROL_H
#define __SCX_RVV_LEASE_CONTROL_H

#ifndef __VMLINUX_H__
#include <linux/types.h>
#endif

#define RVV_LEASE_CONTROL_MAGIC       0x524c4354U
#define RVV_LEASE_CONTROL_ABI_VERSION 4U
#define RVV_LEASE_CONTROL_WORKERS     32U
#define RVV_LEASE_CONTROL_TEAMS       8U
#define RVV_LEASE_MAX_CPUS            8U
#define RVV_LEASE_MAX_ACTIVE_WORKERS  8U
#define RVV_LEASE_WORKER_PAGE_SIZE    4096U
#define RVV_LEASE_CONTROL_DEFAULT_DIR "/sys/fs/bpf/scx_rvv_dirty_lease"
#define RVV_LEASE_CONFIG_MAP_NAME     "config"
#define RVV_LEASE_WORKER_MAP_NAME     "workers"

enum rvv_lease_control_mode {
	RVV_LEASE_CONTROL_OFF = 0,
	RVV_LEASE_CONTROL_OBSERVE = 1,
	RVV_LEASE_CONTROL_ACTIVE = 2,
};

enum rvv_lease_model_hint_flags {
	RVV_LEASE_MODEL_ACTIVE = 1U << 0,
	RVV_LEASE_MODEL_LATENCY_SENSITIVE = 1U << 1,
};

enum rvv_lease_phase {
	RVV_LEASE_PHASE_IDLE = 0,
	RVV_LEASE_PHASE_PREFILL = 1,
	RVV_LEASE_PHASE_DECODE = 2,
	RVV_LEASE_PHASE_MIXED = 3,
};

struct rvv_lease_worker_header {
	__u32 magic;
	__u32 abi_version;
	__u32 active;
	__u32 target_tgid;
	__u32 max_workers;
	__u32 loader_pid;
	__u32 team_id;
	__u32 hint_flags;
	__u64 session_id;
	__u64 policy_epoch;
	__u64 hint_epoch;
	__u32 worker_limit;
	__u32 registered_mask;
} __attribute__((aligned(64)));

struct rvv_lease_worker_slot {
	__u32 sequence;
	__u32 tid;
	__u32 tgid;
	__u32 ith;
	__u32 n_threads;
	__u32 flags;
	__u64 session_id;
	__u64 membership_epoch;
	__u64 reserved64[3];
} __attribute__((aligned(64)));

struct rvv_lease_team_hint {
	__u32 sequence;
	__u32 phase;
	__u32 active_workers;
	__u32 flags;
	__u32 prompt_tokens;
	__u32 decode_tokens;
	__u64 membership_epoch;
	__u64 phase_epoch;
	__u64 begin_ns;
	__u64 expires_ns;
	__u64 session_id;
} __attribute__((aligned(64)));

struct rvv_lease_worker_page {
	struct rvv_lease_worker_header header;
	struct rvv_lease_team_hint team_hint;
	struct rvv_lease_worker_slot workers[RVV_LEASE_CONTROL_WORKERS];
	__u8 reserved[RVV_LEASE_WORKER_PAGE_SIZE -
			      sizeof(struct rvv_lease_worker_header) -
			      sizeof(struct rvv_lease_team_hint) -
			      sizeof(struct rvv_lease_worker_slot) *
		      RVV_LEASE_CONTROL_WORKERS];
};

#ifndef __VMLINUX_H__
_Static_assert(sizeof(struct rvv_lease_worker_header) == 64,
	       "lease worker header must occupy one cache line");
_Static_assert(sizeof(struct rvv_lease_worker_slot) == 64,
		       "lease worker slot must occupy one cache line");
_Static_assert(sizeof(struct rvv_lease_team_hint) == 64,
		       "lease team hint must occupy one cache line");
_Static_assert(sizeof(struct rvv_lease_worker_page) == RVV_LEASE_WORKER_PAGE_SIZE,
	       "lease worker page must occupy one page");
#endif

#endif /* __SCX_RVV_LEASE_CONTROL_H */
