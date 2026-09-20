// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE
#include "control.h"
#include "allocation.h"
#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sched.h>
#include <sys/epoll.h>
#include <sys/resource.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/timerfd.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#ifndef DT_NO_BPF
#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include "control.skel.h"
#endif

enum mode { FIXED8, FIXED4, SPLIT4, DYNAMIC, MANUAL };
struct peer {
    int fd, pidfd;
    pid_t pid;
    bool hello, active, pending, held, members, revoked;
    bool decode_protected;
    uint32_t limit, generation;
    uint32_t phase, prompt_tokens, decode_tokens;
    struct dt_target target;
    uint64_t session, last_request, epoch, order, queued_ns;
    uint64_t decode_wait_started_ns, decode_promoted_ns;
    struct dt_msg request, grant;
    uint32_t tids[DT_WORKERS];
    struct dt_msg output[64];
    unsigned output_head, output_count;
};
static struct peer peers[DT_TEAMS];
static int epfd;
static enum mode policy;
static bool use_scx, fatal;
static bool decode_floor_on_prefill;
static unsigned lifetime, allocation;
static uint64_t order, grants_total;
static uint32_t next_generation;
static uint64_t decode_wait_budget_ns = 2000000000ULL;
static FILE *events;
#ifndef DT_NO_BPF
static struct control_bpf *bpf;
static struct bpf_link *scx_link;
#endif

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}
static void event(const char *kind, unsigned slot, const struct peer *p)
{
    if (!events) return;
    uint64_t now = now_ns();
    uint64_t wait = p->decode_wait_started_ns ? now - p->decode_wait_started_ns : 0;
    uint64_t over = decode_wait_budget_ns && wait > decode_wait_budget_ns ?
                    wait - decode_wait_budget_ns : 0;
    uint64_t recovery = p->decode_promoted_ns ? now - p->decode_promoted_ns : 0;
    uint64_t epoch = p->epoch + (lifetime == DT_PERSISTENT && !strcmp(kind, "REQUEST"));
    fprintf(events, "%" PRIu64 ",%s,%u,%d,%" PRIu64 ",%u,%" PRIu64
            ",%u,%u,%u,%u,%" PRIu64 ",%u,%" PRIu64 ",%" PRIu64 ",%" PRIu64 "\n",
            now, kind, slot, p->pid, epoch, p->grant.workers, p->grant.cpu_mask,
            p->phase, p->prompt_tokens, p->decode_tokens, p->target.workers, p->target.mask,
            p->decode_protected, wait, over, recovery);
    fflush(events);
}
static void finish_decode_wait(unsigned slot, const char *kind)
{
    struct peer *p = &peers[slot];
    if (p->decode_wait_started_ns) event(kind, slot, p);
    p->decode_wait_started_ns = p->decode_promoted_ns = 0;
}
static void update_decode_wait(unsigned slot)
{
    struct peer *p = &peers[slot];
    if (allocation != DT_PREFILL_FIRST) return;
    if (p->fd < 0 || !p->active || p->phase != DT_DECODE) {
        finish_decode_wait(slot, "DECODE_WAIT_CANCEL");
        return;
    }
    /* A revoked but still held graph can run until its safe boundary. Its
       compute time is NOT no-grant wait. Start only after RELEASE/ACQUIRE. */
    if (!p->decode_wait_started_ns && p->pending && !p->held) {
        p->decode_wait_started_ns = now_ns();
        event("DECODE_WAIT", slot, p);
    }
    if (p->decode_wait_started_ns && !p->decode_protected && decode_wait_budget_ns &&
        now_ns() - p->decode_wait_started_ns >= decode_wait_budget_ns) {
        p->decode_protected = true;
        p->decode_promoted_ns = now_ns();
        event("DECODE_PROTECT", slot, p);
    }
}
static uint64_t peer_tag(unsigned slot, bool pidfd)
{
    return ((uint64_t)peers[slot].generation << 32) | (10 + slot * 2 + pidfd);
}
static bool current_peer_event(uint64_t tag)
{
    uint32_t low = tag;
    if (low < 10 || low >= 10 + DT_TEAMS * 2) return false;
    unsigned slot = (low - 10) / 2;
    return peers[slot].pidfd >= 0 && peers[slot].generation == (tag >> 32);
}
static int watch(int fd, uint64_t tag)
{
    struct epoll_event ev = {.events = EPOLLIN | EPOLLRDHUP, .data.u64 = tag};
    return epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);
}
static int publish(unsigned slot, bool ready)
{
#ifndef DT_NO_BPF
    struct peer *p = &peers[slot];
    if (use_scx) {
        struct dt_bpf_grant g = { .session = p->session, .epoch = p->epoch,
            .cpu_mask = p->grant.cpu_mask, .tgid = p->pid,
            .workers = p->grant.workers, .ready = ready, .pin = p->grant.pin };
        if (bpf_map_update_elem(bpf_map__fd(bpf->maps.grants), &slot, &g, BPF_ANY))
            return -1;
    }
#else
    (void)slot; (void)ready;
#endif
    return 0;
}
static int clear_members(unsigned slot)
{
    struct peer *p = &peers[slot];
    if (publish(slot, false)) return -1;
    for (unsigned i = 0; i < DT_WORKERS; i++) {
#ifndef DT_NO_BPF
        if (use_scx && p->tids[i] &&
            bpf_map_delete_elem(bpf_map__fd(bpf->maps.members), &p->tids[i]) && errno != ENOENT)
            return -1;
#endif
        p->tids[i] = 0;
    }
    p->members = false;
    return 0;
}
static void disconnected(unsigned slot)
{
    struct peer *p = &peers[slot];
    if (p->fd >= 0) { close(p->fd); p->fd = -1; }
    p->pending = false;
    p->active = false;
    p->output_count = 0;
    /* Held grants stay quarantined until the original process has exited. */
    event(p->held ? "QUARANTINE" : "DISCONNECT", slot, p);
}
static bool flush_output(unsigned slot)
{
    struct peer *p = &peers[slot];
    while (p->fd >= 0 && p->output_count) {
        ssize_t n = send(p->fd, &p->output[p->output_head], sizeof(struct dt_msg),
                         MSG_NOSIGNAL | MSG_DONTWAIT);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        if (n != sizeof(struct dt_msg)) { disconnected(slot); return false; }
        event("WIRE_TX", slot, p);
        p->output_head = (p->output_head + 1) % 64;
        p->output_count--;
    }
    if (p->fd < 0) return false;
    struct epoll_event ev = { .events = EPOLLIN | EPOLLRDHUP |
        (p->output_count ? EPOLLOUT : 0), .data.u64 = peer_tag(slot, false) };
    if (epoll_ctl(epfd, EPOLL_CTL_MOD, p->fd, &ev)) { fatal = true; return false; }
    return true;
}
static bool reply(unsigned slot, struct dt_msg *msg, unsigned status)
{
    struct peer *p = &peers[slot];
    msg->status = status;
    msg->session = p->session;
    msg->slot = slot;
    if (p->fd < 0) return false;
    if (p->output_count == 64) {
        fprintf(stderr, "output queue exhausted slot=%u\n", slot);
        fatal = true; return false;
    }
    p->output[(p->output_head + p->output_count++) % 64] = *msg;
    return flush_output(slot);
}
static void reject(unsigned slot, struct dt_msg *msg)
{
    fprintf(stderr, "REJECT slot=%u op=%u request=%" PRIu64 " epoch=%" PRIu64 "\n",
            slot, msg->op, msg->request, msg->epoch);
    reply(slot, msg, EPROTO);
    disconnected(slot);
}
static void serve_waiters(void)
{
    struct dt_target targets[DT_TEAMS] = {0};
    unsigned active = 0;
    for (unsigned i = 0; i < DT_TEAMS; i++) {
        struct peer *p = &peers[i];
        if (lifetime == DT_PERSISTENT ? (p->active || (p->fd < 0 && p->held)) :
            (p->active || p->pending || p->held)) active++;
    }
    if (lifetime == DT_PERSISTENT) {
        struct dt_demand demands[DT_TEAMS] = {0};
        uint64_t available = 255;
        bool has_prefill = false;
        if (allocation == DT_PREFILL_FIRST) {
            for (unsigned i = 0; i < DT_TEAMS; i++) {
                struct peer *p = &peers[i];
                if (p->fd >= 0 && p->active &&
                    (p->phase == DT_PREFILL || p->phase == DT_MIXED)) {
                    has_prefill = true;
                    break;
                }
            }
        }
        for (unsigned i = 0; i < DT_TEAMS; i++) {
            struct peer *p = &peers[i];
            update_decode_wait(i);
            if (p->fd < 0 && p->held) available &= ~p->grant.cpu_mask;
            demands[i] = (struct dt_demand){.eligible = p->fd >= 0 && p->active,
                .limit = p->request.max_workers ? p->request.max_workers : p->limit,
                .retained_mask = p->grant.cpu_mask};
            if (allocation == DT_PREFILL_FIRST) {
                demands[i].eligible &= p->phase != DT_UNKNOWN;
                demands[i].priority = p->phase == DT_PREFILL || p->phase == DT_MIXED;
                demands[i].minimum = p->phase == DT_DECODE &&
                    (p->decode_protected || (decode_floor_on_prefill && has_prefill));
            }
        }
        dt_allocate(demands, targets, DT_TEAMS, available);
        for (unsigned i = 0; i < DT_TEAMS; i++) {
            struct peer *p = &peers[i];
            if (p->target.workers != targets[i].workers || p->target.mask != targets[i].mask) {
                p->target = targets[i];
                event("TARGET", i, p);
            }
            if (!p->held || p->revoked || p->fd < 0) continue;
            if (p->active && p->grant.workers == targets[i].workers &&
                p->grant.cpu_mask == targets[i].mask) continue;
            struct dt_msg notification = p->grant;
            notification.op = DT_REVOKE;
            notification.request = 0;
            notification.workers = targets[i].workers;
            notification.cpu_mask = targets[i].mask;
            p->revoked = true;
            event("REVOKE", i, p);
            reply(i, &notification, 0);
        }
    }
    for (;;) {
        int first = -1;
        uint64_t occupied = 0;
        for (unsigned i = 0; i < DT_TEAMS; i++) {
            struct peer *p = &peers[i];
            if (p->held && p->grant.pin) occupied |= p->grant.cpu_mask;
        }
        for (unsigned i = 0; i < DT_TEAMS; i++) {
            struct peer *p = &peers[i];
            if (lifetime == DT_PERSISTENT &&
                (!targets[i].workers || (targets[i].mask & occupied))) continue;
            if (p->pending && (first < 0 || p->order < peers[first].order)) first = i;
        }
        if (first < 0) return;
        struct peer *p = &peers[first];
        unsigned workers = 8;
        uint64_t mask = 255;
        bool pin = policy == SPLIT4 || policy == DYNAMIC;
        if (lifetime == DT_PERSISTENT) {
            workers = targets[first].workers;
            mask = targets[first].mask;
        } else {
            if (policy == FIXED4 || policy == SPLIT4 || (policy == DYNAMIC && active > 1))
                workers = 4;
            if (policy == MANUAL) {
                const unsigned widths[] = {8, 4, 2, 8};
                workers = widths[(p->epoch - 1) % 4];
            }
            if (pin && workers == 4) mask = 15ULL << (first * 4);
            if (workers > p->request.max_workers) workers = p->request.max_workers;
        }
        if (pin && (mask & occupied)) return;
        p->grant = p->request;
        if (lifetime == DT_PERSISTENT) {
            p->grant.op = DT_GRANT;
            p->grant.request = 0;
            p->grant.epoch = ++p->epoch;
        }
        p->grant.workers = workers;
        p->grant.cpu_mask = mask;
        p->grant.pin = pin;
        p->grant.session = p->session;
        p->grant.slot = first;
        p->held = true;
        p->revoked = false;
        p->pending = false;
        if (publish(first, false)) { fatal = true; return; }
        event("GRANT", first, p);
        grants_total++;
        reply(first, &p->grant, 0);
    }
}
static void handle(unsigned slot)
{
    struct peer *p = &peers[slot];
    struct dt_msg msg = {0};
    ssize_t n = recv(p->fd, &msg, sizeof(msg), MSG_TRUNC);
    if (n < 0 && (errno == EAGAIN || errno == EINTR)) return;
    if (n <= 0) { disconnected(slot); return; }
    event("WIRE_RX", slot, p);
    if (n != sizeof(msg) || msg.magic != DT_MAGIC || msg.version != DT_VERSION ||
        msg.status || msg.request != p->last_request + 1 ||
        (!p->hello && msg.op != DT_HELLO) ||
        (p->hello && (msg.session != p->session || msg.slot != slot))) {
        reject(slot, &msg); return;
    }
    p->last_request = msg.request;
    switch (msg.op) {
    case DT_HELLO:
        if (p->hello || !msg.max_workers || msg.max_workers > DT_WORKERS || msg.epoch)
            break;
        p->hello = true;
        p->limit = msg.max_workers;
        msg.pin = lifetime;
        msg.allocation = allocation;
        reply(slot, &msg, 0);
        event("HELLO", slot, p);
        return;
    case DT_ACTIVITY:
        if (msg.workers > 1) break;
        if (lifetime == DT_GRAPH && (p->pending || p->held || msg.epoch != p->epoch)) break;
        p->active = msg.workers;
        if (!p->active) {
            finish_decode_wait(slot, "DECODE_WAIT_CANCEL");
            if (p->decode_protected) {
                p->decode_protected = false;
                event("DECODE_UNPROTECT", slot, p);
            }
            p->pending = false;
            p->phase = DT_UNKNOWN;
        }
        reply(slot, &msg, 0);
        event(p->active ? "ACTIVE" : "IDLE", slot, p);
        return;
    case DT_ACQUIRE:
        if (!p->active || p->pending || p->held ||
            msg.epoch != p->epoch + (lifetime == DT_GRAPH) ||
            !msg.max_workers || msg.max_workers > p->limit) break;
        if (allocation == DT_PREFILL_FIRST) {
            if (!dt_valid_phase(msg.phase, msg.prompt_tokens, msg.decode_tokens)) break;
            p->phase = msg.phase;
            p->prompt_tokens = msg.prompt_tokens;
            p->decode_tokens = msg.decode_tokens;
            event("PHASE", slot, p);
        }
        p->epoch = msg.epoch;
        p->request = msg;
        p->pending = true;
        p->order = ++order;
        p->queued_ns = now_ns();
        event("REQUEST", slot, p);
        if (lifetime == DT_PERSISTENT) reply(slot, &msg, 0);
        return;
    case DT_PHASE:
        if (allocation != DT_PREFILL_FIRST || !p->active || (!p->held && !p->pending) ||
            !dt_valid_phase(msg.phase, msg.prompt_tokens, msg.decode_tokens)) break;
        /* A queued asynchronous GRANT may have advanced epoch before this RPC.
           Session + request sequence authenticate intent; it does not release ownership. */
        p->phase = msg.phase;
        p->prompt_tokens = msg.prompt_tokens;
        p->decode_tokens = msg.decode_tokens;
        p->request.phase = msg.phase;
        p->request.prompt_tokens = msg.prompt_tokens;
        p->request.decode_tokens = msg.decode_tokens;
        event("PHASE", slot, p);
        /* The caller must consume any required REVOKE before PHASE ACK. */
        serve_waiters();
        reply(slot, &msg, 0);
        return;
    case DT_MEMBERS:
        if (!p->held || p->members || msg.epoch != p->epoch ||
            !msg.workers || msg.workers > p->grant.workers ||
            (lifetime == DT_GRAPH && msg.workers != p->grant.workers)) break;
        for (unsigned i = 0; i < msg.workers; i++) {
            char path[96];
            snprintf(path, sizeof(path), "/proc/%d/task/%u", p->pid, msg.tids[i]);
            if (!msg.tids[i] || access(path, F_OK)) goto invalid;
            for (unsigned j = 0; j < i; j++)
                if (msg.tids[i] == msg.tids[j]) goto invalid;
        }
        for (unsigned i = 0, cpu = 0; i < msg.workers; i++, cpu++) {
            while (cpu < DT_WORKERS && !(p->grant.cpu_mask & (1ULL << cpu))) cpu++;
            if (cpu >= DT_WORKERS) { fatal = true; return; }
            p->tids[i] = msg.tids[i];
#ifndef DT_NO_BPF
            if (use_scx) {
                uint64_t start_boottime;
                if (bpf_map_lookup_elem(bpf_map__fd(bpf->maps.identities), &msg.tids[i],
                                        &start_boottime) || !start_boottime) {
                    fatal = true; return;
                }
                struct dt_bpf_member member = { .session = p->session, .epoch = p->epoch,
                                                .start_boottime = start_boottime,
                                                .slot = slot, .cpu = cpu };
                if (bpf_map_update_elem(bpf_map__fd(bpf->maps.members), &msg.tids[i],
                                        &member, BPF_NOEXIST)) { fatal = true; return; }
            }
#endif
        }
        if (publish(slot, true)) { fatal = true; return; }
        p->members = true;
        event("MEMBERS", slot, p);
        /* GRANT alone is not readiness. MEMBERS authorizes computation of this
           graph even if REVOKE raced with registration: the client can only
           release at its next boundary. Do not charge that compute as waiting.
           An already revoked grant discarded before registration has no MEMBERS. */
        if (p->phase == DT_DECODE)
            finish_decode_wait(slot, "DECODE_RESUME");
        reply(slot, &msg, 0);
        return;
    case DT_SHRINK:
        /* The old mask remains owned until this boundary ACK. A target may
           have changed since REVOKE: any strict subset is safe to return;
           serve_waiters will issue another REVOKE if it no longer fits.
           No published members may span this in-place grant update. */
        if (lifetime != DT_PERSISTENT || !p->held || !p->revoked || p->members ||
            !p->grant.pin || msg.epoch != p->epoch || !msg.workers ||
            msg.workers >= p->grant.workers ||
            __builtin_popcountll(msg.cpu_mask) != (int)msg.workers ||
            (msg.cpu_mask & ~p->grant.cpu_mask)) break;
        p->grant.workers = msg.workers;
        p->grant.cpu_mask = msg.cpu_mask;
        p->revoked = false;
        if (publish(slot, false)) { fatal = true; return; }
        event("SHRINK", slot, p);
        reply(slot, &msg, 0);
        return;
    case DT_RELEASE:
        /* Also permits releasing a reserved graph that failed before team creation. */
        if (!p->held || p->members || msg.epoch != p->epoch) break;
        p->held = false;
        event("RELEASE", slot, p);
        reply(slot, &msg, 0);
        if (lifetime == DT_PERSISTENT && p->active && p->fd >= 0) {
            p->pending = true;
            p->order = ++order;
            p->queued_ns = now_ns();
            event("REQUEST", slot, p);
        }
        return;
    case DT_UNMEMBERS:
        if (!p->held || !p->members || msg.epoch != p->epoch) break;
        if (clear_members(slot)) { fatal = true; return; }
        event("UNMEMBERS", slot, p);
        reply(slot, &msg, 0);
        return;
    default: break;
    }
invalid:
    reject(slot, &msg);
}
static int accept_peer(int listener)
{
    int fd = accept4(listener, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (fd < 0) return errno == EAGAIN ? 0 : -1;
    struct ucred cred;
    socklen_t len = sizeof(cred);
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) || cred.uid != getuid()) {
        close(fd); return 0;
    }
    int slot = -1;
    unsigned capacity = lifetime == DT_PERSISTENT ? DT_TEAMS : DT_GRAPH_TEAMS;
    for (unsigned i = 0; i < capacity; i++) {
        if (peers[i].pid == cred.pid) { close(fd); return 0; }
        if (peers[i].pidfd < 0 && slot < 0) slot = i;
    }
    if (slot < 0) {
        fprintf(stderr, "REJECT connection capacity=%u pid=%d\n", capacity, cred.pid);
        close(fd); return 0;
    }
    struct peer *p = &peers[slot];
    memset(p, 0, sizeof(*p));
    p->fd = fd;
    p->pidfd = -1;
    p->pid = cred.pid;
    /* Avoid aliasing an old event if a slot is reused within an epoll batch. */
    if (next_generation == UINT32_MAX) { close(fd); p->fd = -1; return -1; }
    p->generation = ++next_generation;
    p->pidfd = syscall(SYS_pidfd_open, cred.pid, 0);
    p->session = now_ns() ^ ((uint64_t)cred.pid << 32) ^ ++order;
    if (!p->session) p->session = 1;
    if (p->pidfd < 0 || watch(fd, peer_tag(slot, false)) || watch(p->pidfd, peer_tag(slot, true))) {
        close(fd);
        if (p->pidfd >= 0) close(p->pidfd);
        p->fd = p->pidfd = -1;
        p->pid = 0;
        return -1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    const char *socket_path = NULL, *mode = "dynamic", *log = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--socket") && i + 1 < argc) socket_path = argv[++i];
        else if (!strcmp(argv[i], "--mode") && i + 1 < argc) mode = argv[++i];
        else if (!strcmp(argv[i], "--events") && i + 1 < argc) log = argv[++i];
        else if (!strcmp(argv[i], "--decode-wait-ms") && i + 1 < argc) {
            const char *value = argv[++i];
            char *end;
            errno = 0;
            unsigned long ms = strtoul(value, &end, 10);
            if (errno || !*value || *value == '-' || *end || ms > 3600000) {
                fprintf(stderr, "decode-wait-ms must be 0..3600000 (0 disables protection)\n");
                return 2;
            }
            decode_wait_budget_ns = (uint64_t)ms * 1000000;
        }
        else if (!strcmp(argv[i], "--decode-floor-on-prefill"))
            decode_floor_on_prefill = true;
        else if (!strcmp(argv[i], "--grant-lifetime") && i + 1 < argc) {
            const char *value = argv[++i];
            if (!strcmp(value, "persistent")) lifetime = DT_PERSISTENT;
            else if (strcmp(value, "graph")) return 2;
        }
        else if (!strcmp(argv[i], "--allocation-policy") && i + 1 < argc) {
            const char *value = argv[++i];
            if (!strcmp(value, "prefill-first")) allocation = DT_PREFILL_FIRST;
            else if (strcmp(value, "equal")) return 2;
        }
        else if (!strcmp(argv[i], "--scx")) use_scx = true;
        else { fprintf(stderr, "unknown argument: %s\n", argv[i]); return 2; }
    }
    if (!socket_path || strlen(socket_path) >= sizeof(((struct sockaddr_un *)0)->sun_path)) return 2;
    if (!strcmp(mode, "fixed8")) policy = FIXED8;
    else if (!strcmp(mode, "fixed4")) policy = FIXED4;
    else if (!strcmp(mode, "split4")) policy = SPLIT4;
    else if (!strcmp(mode, "dynamic")) policy = DYNAMIC;
    else if (!strcmp(mode, "manual")) policy = MANUAL;
    else return 2;
    if (lifetime == DT_PERSISTENT && policy != DYNAMIC) {
        fprintf(stderr, "persistent lifetime requires dynamic policy\n"); return 2;
    }
    if (allocation == DT_PREFILL_FIRST && lifetime != DT_PERSISTENT) {
        fprintf(stderr, "prefill-first requires persistent lifetime\n"); return 2;
    }
    if (decode_floor_on_prefill && allocation != DT_PREFILL_FIRST) {
        fprintf(stderr, "decode-floor-on-prefill requires prefill-first allocation\n"); return 2;
    }
    for (unsigned i = 0; i < DT_TEAMS; i++) peers[i].fd = peers[i].pidfd = -1;
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (log) {
        events = fopen(log, "wx");
        if (!events) { perror("events"); return 1; }
        fputs("time_ns,event,slot,pid,epoch,workers,cpu_mask,phase,prompt_tokens,decode_tokens,target_workers,target_mask,decode_protected,decode_wait_ns,decode_over_budget_ns,decode_recovery_ns\n", events);
    }
    sigset_t mask;
    sigemptyset(&mask); sigaddset(&mask, SIGINT); sigaddset(&mask, SIGTERM);
    if (sigprocmask(SIG_BLOCK, &mask, NULL)) return 1;
    epfd = epoll_create1(EPOLL_CLOEXEC);
    int sigfd = signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK);
    int timer = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    struct itimerspec interval = { .it_interval.tv_nsec = 100000000, .it_value.tv_nsec = 100000000 };
    int listener = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (epfd < 0 || sigfd < 0 || timer < 0 || listener < 0 ||
        timerfd_settime(timer, 0, &interval, NULL)) { perror("setup"); return 1; }
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    strcpy(addr.sun_path, socket_path);
    umask(0077);
    /* Never unlink another instance's socket to make bind succeed. */
    if (bind(listener, (struct sockaddr *)&addr, sizeof(addr))) { perror("bind"); return 1; }
    if (listen(listener, DT_TEAMS) || watch(listener, 0) || watch(sigfd, 1) || watch(timer, 2)) {
        perror("listen/watch"); fatal = true; goto out;
    }
#ifndef DT_NO_BPF
    if (use_scx) {
        struct rlimit limit = {RLIM_INFINITY, RLIM_INFINITY};
        if (setrlimit(RLIMIT_MEMLOCK, &limit)) { fatal = true; goto out; }
        bpf = control_bpf__open();
        if (!bpf || bpf_program__set_flags(bpf->progs.dtc_init, BPF_F_SLEEPABLE) ||
            control_bpf__load(bpf)) { fatal = true; goto out; }
        scx_link = bpf_map__attach_struct_ops(bpf->maps.dtc_ops);
        if (libbpf_get_error(scx_link)) { scx_link = NULL; fatal = true; goto out; }
    }
#else
    if (use_scx) { fprintf(stderr, "host build has no BPF support\n"); fatal = true; goto out; }
#endif
    printf("READY socket=%s mode=%s lifetime=%s allocation=%s decode_wait_ms=%" PRIu64
           " decode_floor_on_prefill=%d scx=%d pid=%d\n", socket_path, mode,
           lifetime == DT_PERSISTENT ? "persistent" : "graph",
           allocation == DT_PREFILL_FIRST ? "prefill-first" : "equal",
           allocation == DT_PREFILL_FIRST ? decode_wait_budget_ns / 1000000 : 0,
           decode_floor_on_prefill, use_scx, getpid());
    bool stopping = false;
    while (!fatal && !stopping) {
        struct epoll_event ev[3 + 2 * DT_TEAMS];
        int n = epoll_wait(epfd, ev, 3 + 2 * DT_TEAMS, -1);
        if (n < 0) { if (errno == EINTR) continue; fatal = true; break; }
        for (int i = 0; i < n && !fatal && !stopping; i++) {
            unsigned tag = (uint32_t)ev[i].data.u64;
            if (tag == 0) { if (accept_peer(listener)) fatal = true; }
            else if (tag == 1) stopping = true;
            else if (tag == 2) {
                uint64_t count;
                if (read(timer, &count, sizeof(count)) != sizeof(count)) { fatal = true; break; }
#ifndef DT_NO_BPF
                if (use_scx && __atomic_load_n(&bpf->bss->exit_kind, __ATOMIC_ACQUIRE)) {
                    fprintf(stderr, "scheduler exit: %u %s\n", bpf->bss->exit_kind, bpf->bss->exit_reason);
                    fatal = true; break;
                }
#endif
                for (unsigned j = 0; j < DT_TEAMS; j++) {
                    if (lifetime == DT_GRAPH && peers[j].pending &&
                        now_ns() - peers[j].queued_ns > 30000000000ULL) {
                        fprintf(stderr, "pending timeout slot=%u\n", j);
                        fatal = true;
                    }
                }
            } else {
                if (!current_peer_event(ev[i].data.u64)) continue;
                unsigned slot = (tag - 10) / 2;
                struct peer *p = &peers[slot];
                if (tag & 1) {
                    if (clear_members(slot)) { fatal = true; break; }
                    event("EXIT", slot, p);
                    if (p->fd >= 0) close(p->fd);
                    if (p->pidfd >= 0) close(p->pidfd);
                    memset(p, 0, sizeof(*p));
                    p->fd = p->pidfd = -1;
                } else if (p->fd >= 0) {
                    if (ev[i].events & EPOLLOUT) flush_output(slot);
                    if (p->fd >= 0 && (ev[i].events & (EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLERR)))
                        handle(slot);
                }
            }
        }
        if (!stopping && !fatal) serve_waiters();
    }
out:
    for (unsigned i = 0; i < DT_TEAMS; i++) {
        struct peer *p = &peers[i];
        if (p->held || p->pending) {
            fatal = true;
            /* Abort only registered processes whose experiment is still in flight. */
            if (p->pidfd >= 0) syscall(SYS_pidfd_send_signal, p->pidfd, SIGTERM, NULL, 0);
        }
        if (p->fd >= 0) close(p->fd);
        if (p->pidfd >= 0) close(p->pidfd);
    }
#ifndef DT_NO_BPF
    if (bpf && use_scx && bpf->bss) {
        printf("BPF enqueue=%llu placed=%llu running=%llu wrong_cpu=%llu stale_identity=%llu\n",
               (unsigned long long)bpf->bss->enqueue_count, (unsigned long long)bpf->bss->placed_count,
               (unsigned long long)bpf->bss->running_count, (unsigned long long)bpf->bss->wrong_cpu_count,
               (unsigned long long)bpf->bss->stale_identity_count);
        if (bpf->bss->wrong_cpu_count) fatal = true;
    }
    bpf_link__destroy(scx_link);
    control_bpf__destroy(bpf);
#endif
    close(listener); close(timer); close(sigfd); close(epfd);
    unlink(socket_path);
    if (events) fclose(events);
    printf("STOP grants=%" PRIu64 " status=%s\n", grants_total, fatal ? "FAIL" : "OK");
    return fatal ? 1 : 0;
}
