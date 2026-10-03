/* SPDX-License-Identifier: GPL-2.0 */
#define _GNU_SOURCE
#include "client.h"
#include <errno.h>
#include <poll.h>
#include <stdbool.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

_Static_assert(sizeof(struct dt_msg) == 112, "wire layout changed");
static int64_t clock_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
static int wait_fd(int fd, short events, int64_t deadline)
{
    for (;;) {
        int64_t remaining = deadline < 0 ? -1 : deadline - clock_ms();
        if (deadline >= 0 && remaining <= 0) { errno = ETIMEDOUT; return -1; }
        struct pollfd pollfd = {.fd = fd, .events = events};
        int rc = poll(&pollfd, 1, remaining);
        if (rc < 0 && errno == EINTR) continue;
        if (rc < 0) return -1;
        if (!rc) { errno = ETIMEDOUT; return -1; }
        if (pollfd.revents & events) return 0;
        errno = ECONNRESET;
        return -1;
    }
}
static int valid_message(const struct dt_client *c, const struct dt_msg *m)
{
    if (m->magic != DT_MAGIC || m->version != DT_VERSION || m->slot >= DT_TEAMS ||
        !m->session || (c->session && (m->session != c->session || m->slot != c->slot))) {
        errno = EPROTO; return -1;
    }
    if (m->status) { errno = m->status; return -1; }
    return 0;
}
static int valid_grant(const struct dt_msg *m, unsigned maximum)
{
    if (!m->workers || m->workers > maximum || m->workers > DT_WORKERS || m->pin > 1 ||
        !m->cpu_mask || (m->cpu_mask & ~255ULL) ||
        __builtin_popcountll(m->cpu_mask) < (int)m->workers) { errno = EPROTO; return -1; }
    return 0;
}
static int notification(struct dt_client *c, const struct dt_msg *m)
{
    if (c->lifetime != DT_PERSISTENT || m->request || valid_message(c, m)) {
        errno = EPROTO; return -1;
    }
    if (m->op == DT_GRANT) {
        if (c->held || !c->pending || m->epoch != c->epoch + 1 || valid_grant(m, c->maximum)) {
            errno = EPROTO; return -1;
        }
        c->grant = *m;
        c->epoch = m->epoch;
        c->held = true;
        c->revoked = false;
        c->pending = false;
    } else if (m->op == DT_REVOKE) {
        if (!c->held || c->revoked || m->epoch != c->epoch ||
            m->workers > c->maximum || (m->cpu_mask & ~255ULL) ||
            __builtin_popcountll(m->cpu_mask) != (int)m->workers) {
            errno = EPROTO; return -1;
        }
        c->revoke = *m;
        c->revoked = true;
    } else { errno = EPROTO; return -1; }
    return 0;
}
static int exchange(struct dt_client *c, struct dt_msg *m)
{
    m->magic = DT_MAGIC;
    m->version = DT_VERSION;
    m->slot = c->slot;
    m->session = c->session;
    m->epoch = c->epoch;
    m->request = ++c->request;
    const struct dt_msg sent = *m;
    int64_t deadline = clock_ms() + c->timeout_ms;
    for (;;) {
        if (wait_fd(c->fd, POLLOUT, deadline)) return -1;
        ssize_t n = send(c->fd, m, sizeof(*m), MSG_NOSIGNAL | MSG_DONTWAIT);
        if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (n != sizeof(*m)) { if (n >= 0) errno = EPROTO; return -1; }
        break;
    }
    for (;;) {
        if (wait_fd(c->fd, POLLIN, deadline)) return -1;
        ssize_t n = recv(c->fd, m, sizeof(*m), MSG_TRUNC | MSG_DONTWAIT);
        if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (n != sizeof(*m)) { if (n >= 0) errno = EPROTO; return -1; }
        if (!m->request) {
            if (notification(c, m)) return -1;
            continue;
        }
        break;
    }
    if (valid_message(c, m) || m->op != sent.op ||
        m->request != sent.request || m->epoch != sent.epoch) {
        errno = EPROTO; return -1;
    }
    return 0;
}
int dt_connect(struct dt_client *c, const char *path, unsigned maximum)
{
    memset(c, 0, sizeof(*c));
    c->fd = -1;
    c->timeout_ms = DT_TIMEOUT_MS;
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    if (!path || !maximum || maximum > DT_WORKERS || strlen(path) >= sizeof(addr.sun_path)) {
        errno = EINVAL; return -1;
    }
    strcpy(addr.sun_path, path);
    c->fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (c->fd < 0) return -1;
    if (connect(c->fd, (struct sockaddr *)&addr, sizeof(addr))) {
        int saved = errno; dt_close(c); errno = saved; return -1;
    }
    struct dt_msg m = {.op = DT_HELLO, .max_workers = maximum};
    if (exchange(c, &m)) { int saved = errno; dt_close(c); errno = saved; return -1; }
    c->slot = m.slot;
    c->session = m.session;
    c->maximum = maximum;
    c->lifetime = m.pin;
    c->allocation = m.allocation;
    if (c->lifetime > DT_PERSISTENT || c->allocation > DT_PREFILL_FIRST ||
        (c->allocation == DT_PREFILL_FIRST && c->lifetime != DT_PERSISTENT)) {
        dt_close(c); errno = EPROTO; return -1;
    }
    return 0;
}
void dt_close(struct dt_client *c)
{
    if (c->fd >= 0) close(c->fd);
    c->fd = -1;
}
int dt_activity(struct dt_client *c, int active)
{
    struct dt_msg m = {.op = DT_ACTIVITY, .workers = !!active};
    if (exchange(c, &m)) return -1;
    c->active = !!active;
    if (!active) { c->pending = false; c->phase = DT_UNKNOWN; }
    return 0;
}
int dt_phase(struct dt_client *c, unsigned phase, unsigned prompt, unsigned decode)
{
    if (c->allocation == DT_EQUAL) return 0;
    if (!c->active || !dt_valid_phase(phase, prompt, decode)) { errno = EINVAL; return -1; }
    if (phase != c->phase && (c->held || c->pending)) {
        struct dt_msg m = {.op = DT_PHASE, .phase = phase,
                          .prompt_tokens = prompt, .decode_tokens = decode};
        if (exchange(c, &m)) return -1;
    }
    c->phase = phase;
    c->prompt_tokens = prompt;
    c->decode_tokens = decode;
    return 0;
}
int dt_request(struct dt_client *c, unsigned maximum)
{
    if (!maximum || maximum > DT_WORKERS || c->held || c->pending) { errno = EINVAL; return -1; }
    if (c->allocation == DT_PREFILL_FIRST &&
        !dt_valid_phase(c->phase, c->prompt_tokens, c->decode_tokens)) { errno = EINVAL; return -1; }
    struct dt_msg m = {.op = DT_ACQUIRE, .max_workers = maximum,
        .phase = c->phase, .prompt_tokens = c->prompt_tokens, .decode_tokens = c->decode_tokens};
    if (c->lifetime == DT_GRAPH) c->epoch++;
    c->maximum = maximum;
    if (exchange(c, &m)) return -1;
    if (c->lifetime == DT_PERSISTENT) { c->pending = true; return 0; }
    if (valid_grant(&m, maximum)) return -1;
    c->grant = m;
    c->held = true;
    return 0;
}
int dt_poll(struct dt_client *c)
{
    if (c->lifetime == DT_GRAPH) return 0;
    for (;;) {
        struct dt_msg m;
        ssize_t n = recv(c->fd, &m, sizeof(m), MSG_TRUNC | MSG_DONTWAIT);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
        if (n != sizeof(m)) { if (n >= 0) errno = ECONNRESET; return -1; }
        if (notification(c, &m)) return -1;
    }
}
int dt_wait_grant(struct dt_client *c)
{
    while (!c->held) {
        if (!c->pending) { errno = ECANCELED; return -1; }
        if (wait_fd(c->fd, POLLIN, -1) || dt_poll(c)) return -1;
    }
    return 0;
}
int dt_acquire(struct dt_client *c, unsigned maximum)
{
    if (dt_request(c, maximum)) return -1;
    return dt_wait_grant(c);
}
int dt_members(struct dt_client *c, const uint32_t *tids, unsigned count)
{
    if (!count || count > DT_WORKERS) { errno = EINVAL; return -1; }
    struct dt_msg m = {.op = DT_MEMBERS, .workers = count};
    memcpy(m.tids, tids, count * sizeof(*tids));
    return exchange(c, &m);
}
int dt_unmembers(struct dt_client *c)
{
    struct dt_msg m = {.op = DT_UNMEMBERS};
    return exchange(c, &m);
}
int dt_release(struct dt_client *c)
{
    struct dt_msg m = {.op = DT_RELEASE};
    if (exchange(c, &m)) return -1;
    c->held = false;
    c->revoked = false;
    /* An active persistent peer is automatically queued for its next grant.
       Idle callers clear this using dt_activity before/after releasing. */
    c->pending = c->lifetime == DT_PERSISTENT && c->active;
    return 0;
}
int dt_shrink(struct dt_client *c)
{
    const struct dt_msg target = c->revoke;
    if (!c->held || !c->revoked || !c->grant.pin || !target.workers ||
        target.workers >= c->grant.workers ||
        (target.cpu_mask & ~c->grant.cpu_mask)) { errno = EINVAL; return -1; }
    struct dt_msg m = {.op = DT_SHRINK, .workers = target.workers,
                       .cpu_mask = target.cpu_mask};
    if (exchange(c, &m)) return -1;
    if (m.workers != target.workers || m.cpu_mask != target.cpu_mask) {
        errno = EPROTO; return -1;
    }
    c->grant.workers = m.workers;
    c->grant.cpu_mask = m.cpu_mask;
    c->revoked = false;
    return 0;
}
int dt_cpu(const struct dt_client *c, unsigned ith)
{
    if (!c->grant.pin || ith >= c->grant.workers) return -1;
    for (unsigned cpu = 0, index = 0; cpu < DT_WORKERS; cpu++) {
        if (!(c->grant.cpu_mask & (1ULL << cpu))) continue;
        if (index++ == ith) return cpu;
    }
    return -1;
}
