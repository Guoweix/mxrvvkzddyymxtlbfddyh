#ifndef DT_CLIENT_H
#define DT_CLIENT_H
#include "control.h"
#include <stdbool.h>

struct dt_client {
    int fd, timeout_ms;
    uint32_t slot;
    unsigned lifetime, maximum;
    unsigned allocation, phase, prompt_tokens, decode_tokens;
    bool held, revoked, pending, active;
    uint64_t session, request, epoch;
    struct dt_msg grant, revoke;
};
int dt_connect(struct dt_client *c, const char *path, unsigned maximum);
void dt_close(struct dt_client *c);
int dt_activity(struct dt_client *c, int active);
/* At a graph boundary, BEFORE waiting. Initial hint rides on ACQUIRE. */
int dt_phase(struct dt_client *c, unsigned phase, unsigned prompt, unsigned decode);
int dt_acquire(struct dt_client *c, unsigned maximum);
/* Persistent: submit without waiting, then poll/wait for asynchronous grant. */
int dt_request(struct dt_client *c, unsigned maximum);
int dt_poll(struct dt_client *c);
int dt_wait_grant(struct dt_client *c);
int dt_members(struct dt_client *c, const uint32_t *tids, unsigned count);
int dt_unmembers(struct dt_client *c);
int dt_release(struct dt_client *c);
/* After UNMEMBERS and restoring only the workers outside this subset. */
int dt_shrink(struct dt_client *c);
/* Return ith's assigned CPU, or -1 for shared/no-pinning grants. */
int dt_cpu(const struct dt_client *c, unsigned ith);
#endif
