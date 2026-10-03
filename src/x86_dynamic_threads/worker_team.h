/* SPDX-License-Identifier: GPL-2.0 */
#ifndef DT_WORKER_TEAM_H
#define DT_WORKER_TEAM_H
#include "client.h"
#include <sched.h>

struct dt_worker;
struct dt_team {
    cpu_set_t allowed;
    struct dt_worker *members[DT_WORKERS], *current[DT_WORKERS];
    int cpus[DT_WORKERS];
    unsigned count;
    bool registered;
};
/* Call before the first grant/pin on the inference thread. */
int dt_team_init(struct dt_team *team);
/* Each actual OpenMP thread records itself, then ALL threads barrier. */
int dt_team_observe(struct dt_team *team, unsigned ith);
/* Single thread calls commit; ALL threads barrier before computing. */
int dt_team_commit(struct dt_team *team, struct dt_client *client, unsigned count);
/* Only at a quiescent graph boundary or while every team member is at a barrier. */
int dt_team_detach(struct dt_team *team, struct dt_client *client);
/* At a graph boundary: retain bindings on a pure shrink, fully release otherwise. */
int dt_team_revoke(struct dt_team *team, struct dt_client *client);
#endif
