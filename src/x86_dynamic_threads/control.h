/* SPDX-License-Identifier: GPL-2.0 */
/* Local SOCK_SEQPACKET protocol. Fixed-width, native-endian on one machine. */
#ifndef DT_CONTROL_H
#define DT_CONTROL_H
#ifdef __VMLINUX_H__
typedef __u64 uint64_t;
typedef __u32 uint32_t;
#else
#include <stdint.h>
#endif

#define DT_MAGIC 0x44544752u
#define DT_VERSION 4u
#define DT_TEAMS 8
#define DT_GRAPH_TEAMS 2
#define DT_WORKERS 8
#define DT_TIMEOUT_MS 30000

enum dt_lifetime { DT_GRAPH = 0, DT_PERSISTENT = 1 };
enum dt_allocation { DT_EQUAL = 0, DT_PREFILL_FIRST = 1 };
/* Matches the backend hint (not the public llama enum, which has AUTO first). */
enum dt_phase { DT_UNKNOWN = 0, DT_PREFILL = 1, DT_DECODE = 2, DT_MIXED = 3 };
static inline int dt_valid_phase(uint32_t phase, uint32_t prompt, uint32_t decode)
{
    return (phase == DT_PREFILL && prompt && !decode) ||
           (phase == DT_DECODE && !prompt && decode) ||
           (phase == DT_MIXED && prompt && decode);
}

enum dt_op {
    DT_HELLO = 1,
    DT_ACTIVITY,
    DT_ACQUIRE,
    DT_MEMBERS,
    DT_RELEASE,
    /* Disable BPF placement while retaining the graph's CPU grant. */
    DT_UNMEMBERS,
    /* Server notifications have request=0. HELLO.pin negotiates lifetime. */
    DT_GRANT,
    DT_REVOKE,
    DT_PHASE,
    /* Quiescent partial return: keep a nonempty subset, in the same epoch. */
    DT_SHRINK,
};
struct dt_msg {
    uint32_t magic, version, op, status;
    uint32_t slot, workers, max_workers, pin;
    uint64_t request, session, epoch, cpu_mask;
    /* REVOKE.workers/cpu_mask describe the target, not the still-held grant.
       SHRINK.workers/cpu_mask describe the retained subset; ACK commits it. */
    uint32_t tids[DT_WORKERS];
    /* HELLO returns allocation; ACQUIRE/PHASE carry work intent. */
    uint32_t allocation, phase, prompt_tokens, decode_tokens;
};

/* Loader is the only writer; BPF only reads. ready is published after members. */
struct dt_bpf_grant {
    uint64_t session, epoch, cpu_mask;
    uint32_t tgid, workers, ready, pin;
};
struct dt_bpf_member {
    uint64_t session, epoch, start_boottime;
    uint32_t slot, cpu;
};
#endif
