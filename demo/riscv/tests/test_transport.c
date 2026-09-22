/* Deterministic socket backpressure and client notification/reply interleaving. */
#define DT_NO_BPF
#define main dt_controller_main
#include "controller.c"
#undef main
#include "client.h"
#include <assert.h>
#include <sys/wait.h>

static void queue_test(void)
{
    int pair[2], size = 1024;
    assert(!socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0, pair));
    assert(!setsockopt(pair[0], SOL_SOCKET, SO_SNDBUF, &size, sizeof(size)));
    epfd = epoll_create1(0);
    memset(&peers[0], 0, sizeof(peers[0]));
    peers[0].fd = pair[0];
    peers[0].session = 1;
    assert(!watch(pair[0], 10));
    for (unsigned i = 1; i <= 32; i++) {
        struct dt_msg msg = {.request = i};
        assert(reply(0, &msg, 0));
    }
    assert(peers[0].output_count > 0 && !fatal);
    unsigned received = 0;
    while (received < 32) {
        struct dt_msg msg;
        ssize_t n = recv(pair[1], &msg, sizeof(msg), MSG_DONTWAIT);
        if (n < 0) assert(errno == EAGAIN);
        else { assert(n == sizeof(msg)); assert(msg.request == ++received); }
        assert(flush_output(0));
    }
    assert(!peers[0].output_count && !fatal);
    close(pair[0]); close(pair[1]); close(epfd);
}
static void interleave_test(void)
{
    int pair[2];
    assert(!socketpair(AF_UNIX, SOCK_SEQPACKET, 0, pair));
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        close(pair[0]);
        struct dt_msg msg;
        assert(recv(pair[1], &msg, sizeof(msg), 0) == sizeof(msg));
        assert(msg.op == DT_MEMBERS);
        struct dt_msg revoke = msg;
        revoke.op = DT_REVOKE; revoke.request = 0; revoke.cpu_mask = 15; revoke.workers = 4;
        assert(send(pair[1], &revoke, sizeof(revoke), 0) == sizeof(revoke));
        assert(send(pair[1], &msg, sizeof(msg), 0) == sizeof(msg));
        /* Deliberately stale notification: never accepted as a fresh grant. */
        revoke.epoch--;
        assert(send(pair[1], &revoke, sizeof(revoke), 0) == sizeof(revoke));
        close(pair[1]);
        _exit(0);
    }
    close(pair[1]);
    struct dt_client c = {.fd = pair[0], .timeout_ms = 1000, .session = 1,
        .epoch = 1, .maximum = 8, .lifetime = DT_PERSISTENT, .held = true,
        .grant = {.workers = 8, .cpu_mask = 255}};
    uint32_t tid = getpid();
    assert(!dt_members(&c, &tid, 1));
    assert(c.revoked);
    int status;
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
    assert(dt_poll(&c) == -1 && errno == EPROTO);
    close(pair[0]);
}
static void phase_interleave_test(void)
{
    int pair[2];
    assert(!socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0, pair));
    struct dt_client c = {.fd = pair[0], .timeout_ms = 1000, .session = 1,
        .maximum = 8, .lifetime = DT_PERSISTENT, .allocation = DT_PREFILL_FIRST,
        .active = true, .pending = true, .phase = DT_PREFILL};
    struct dt_msg msg = {.magic = DT_MAGIC, .version = DT_VERSION, .session = 1,
        .op = DT_GRANT, .epoch = 1, .workers = 8, .cpu_mask = 255, .pin = 1};
    assert(send(pair[1], &msg, sizeof(msg), 0) == sizeof(msg));
    msg.op = DT_REVOKE;
    assert(send(pair[1], &msg, sizeof(msg), 0) == sizeof(msg));
    msg.op = DT_PHASE; msg.epoch = 0; msg.request = 1;
    assert(send(pair[1], &msg, sizeof(msg), 0) == sizeof(msg));
    /* Submitted epoch=0, queued GRANT advances to 1, then REVOKE before ACK. */
    assert(!dt_phase(&c, DT_DECODE, 0, 1));
    assert(c.epoch == 1 && c.held && c.revoked && !c.pending && c.phase == DT_DECODE);
    assert(recv(pair[1], &msg, sizeof(msg), 0) == sizeof(msg));
    assert(msg.op == DT_PHASE && msg.phase == DT_DECODE && msg.epoch == 0);
    for (unsigned i = 1; i <= 10000; i++) assert(!dt_phase(&c, DT_DECODE, 0, i));
    assert(recv(pair[1], &msg, sizeof(msg), 0) < 0 && errno == EAGAIN);
    assert(dt_phase(&c, DT_UNKNOWN, 0, 0) < 0 && errno == EINVAL);
    close(pair[0]); close(pair[1]);
}
int main(void)
{
    peers[7].pidfd = 1;
    peers[7].generation = 42;
    uint64_t retired = peer_tag(7, true);
    assert(current_peer_event(retired));
    peers[7].generation = 43;
    assert(!current_peer_event(retired));
    assert(current_peer_event(peer_tag(7, false)));
    peers[7].pidfd = -1;
    assert(!current_peer_event(peer_tag(7, true)));
    assert(!current_peer_event(10 + DT_TEAMS * 2));
    queue_test();
    interleave_test();
    phase_interleave_test();
    puts("PASS backpressure: ordered bounded queue; async REVOKE during MEMBERS; stale generation rejected");
    puts("PASS PHASE: queued GRANT/REVOKE before ACK, stable 10000 graphs zero phase RPC");
    return 0;
}
