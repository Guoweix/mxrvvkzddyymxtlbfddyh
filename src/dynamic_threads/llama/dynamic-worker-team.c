#define _GNU_SOURCE
#include "dynamic-worker-team.h"
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

struct dt_worker {
    uint32_t tid;
    cpu_set_t original;
    atomic_uint refs;
    bool alive;
};
static pthread_key_t worker_key;
static pthread_once_t key_once = PTHREAD_ONCE_INIT;
static int key_error;
/* Separate from the graph mutex: libgomp can join retiring threads while the
 * graph mutex is held. Holding this lock prevents a TLS destructor from exiting
 * the thread (and thus prevents TID reuse) during a remote affinity restoration. */
static pthread_mutex_t registry = PTHREAD_MUTEX_INITIALIZER;

static void drop(struct dt_worker *worker)
{
    if (worker && atomic_fetch_sub_explicit(&worker->refs, 1, memory_order_acq_rel) == 1)
        free(worker);
}
static void exited(void *value)
{
    struct dt_worker *worker = value;
    pthread_mutex_lock(&registry);
    worker->alive = false;
    pthread_mutex_unlock(&registry);
    drop(worker);
}
static void create_key(void) { key_error = pthread_key_create(&worker_key, exited); }

int dt_team_init(struct dt_team *team)
{
    memset(team, 0, sizeof(*team));
    /* External OpenMP placement competes with this experiment's ownership. */
    const char *bind = getenv("OMP_PROC_BIND");
    if ((bind && strcasecmp(bind, "false")) || getenv("GOMP_CPU_AFFINITY") || getenv("OMP_PLACES")) {
        errno = EINVAL; return -1;
    }
    return sched_getaffinity(0, sizeof(team->allowed), &team->allowed);
}
int dt_team_observe(struct dt_team *team, unsigned ith)
{
    if (ith >= DT_WORKERS || team->current[ith]) { errno = EINVAL; return -1; }
    pthread_once(&key_once, create_key);
    if (key_error) { errno = key_error; return -1; }
    struct dt_worker *worker = pthread_getspecific(worker_key);
    if (!worker) {
        worker = calloc(1, sizeof(*worker));
        if (!worker) return -1;
        worker->tid = syscall(SYS_gettid);
        atomic_init(&worker->refs, 1); /* TLS owns one reference until exit. */
        worker->alive = true;
        if (sched_getaffinity(0, sizeof(worker->original), &worker->original)) {
            free(worker); return -1;
        }
        /* Newly created libgomp workers inherit the pinned master's mask.
         * A persistent team has one baseline, captured before any grant. */
        if (team->count && CPU_COUNT(&worker->original) == 1)
            worker->original = team->allowed;
        int rc = pthread_setspecific(worker_key, worker);
        if (rc) { free(worker); errno = rc; return -1; }
    }
    atomic_fetch_add_explicit(&worker->refs, 1, memory_order_relaxed);
    team->current[ith] = worker;
    return 0;
}
static int unpublish(struct dt_team *team, struct dt_client *client)
{
    /* Never widen affinity while the BPF membership is still published. */
    if (team->registered && dt_unmembers(client)) return -1;
    team->registered = false;
    return 0;
}
static int restore(struct dt_worker *worker)
{
    int result = 0;
    pthread_mutex_lock(&registry);
    if (worker->alive)
        result = sched_setaffinity(worker->tid, sizeof(worker->original), &worker->original);
    pthread_mutex_unlock(&registry);
    return result;
}
static int trim(struct dt_team *team, uint64_t keep)
{
    int result = 0;
    for (unsigned i = 0; i < team->count; i++) {
        struct dt_worker *worker = team->members[i];
        if (worker && !(keep & (1ULL << team->cpus[i])) && restore(worker)) result = -1;
    }
    if (result) return -1; /* Keep ownership; caller must fail closed. */
    for (unsigned i = 0; i < team->count; i++) {
        if (keep & (1ULL << team->cpus[i])) continue;
        drop(team->members[i]);
        team->members[i] = NULL;
    }
    /* Keep index slots until the next actual OpenMP team is observed. */
    if (!keep) team->count = 0;
    return 0;
}
int dt_team_detach(struct dt_team *team, struct dt_client *client)
{
    if (unpublish(team, client)) return -1;
    return trim(team, 0);
}
int dt_team_revoke(struct dt_team *team, struct dt_client *client)
{
    if (!client->held || !client->revoked) { errno = EINVAL; return -1; }
    const struct dt_msg *target = &client->revoke;
    if (client->grant.pin && target->workers && target->workers < client->grant.workers &&
        !(target->cpu_mask & ~client->grant.cpu_mask)) {
        if (unpublish(team, client) || trim(team, target->cpu_mask)) return -1;
        return dt_shrink(client);
    }
    if (dt_team_detach(team, client)) return -1;
    return dt_release(client);
}
int dt_team_commit(struct dt_team *team, struct dt_client *client, unsigned count)
{
    if (!client->held || !count || count > client->grant.workers) { errno = EINVAL; return -1; }
    bool same = team->registered && count == team->count;
    for (unsigned i = 0; i < count; i++) {
        if (!team->current[i]) { errno = EINVAL; return -1; }
        same &= team->current[i] == team->members[i] && team->cpus[i] == dt_cpu(client, i);
    }
    if (same) {
        for (unsigned i = 0; i < count; i++) {
            drop(team->current[i]);
            team->current[i] = NULL;
        }
        return 0;
    }
    if (unpublish(team, client)) return -1;
    bool retained[DT_WORKERS] = {0};
    /* Match live worker identity AND destination CPU, not only team width.
       The allocator keeps the low-CPU prefix on shrink, as does dt_cpu(). */
    for (unsigned old = 0; old < team->count; old++) {
        struct dt_worker *worker = team->members[old];
        if (!worker) continue;
        bool keep = false;
        for (unsigned i = 0; i < count; i++) {
            if (worker == team->current[i] && team->cpus[old] == dt_cpu(client, i)) {
                retained[i] = keep = true;
                break;
            }
        }
        if (!keep && restore(worker)) return -1;
    }
    for (unsigned i = 0; i < team->count; i++) {
        drop(team->members[i]);
        team->members[i] = NULL;
    }
    team->count = 0;
    uint32_t tids[DT_WORKERS] = {0};
    for (unsigned i = 0; i < count; i++) {
        struct dt_worker *worker = team->current[i];
        int cpu = dt_cpu(client, i);
        if (cpu < 0 || !CPU_ISSET(cpu, &team->allowed) || !CPU_ISSET(cpu, &worker->original)) {
            errno = EINVAL; return -1;
        }
        cpu_set_t target;
        CPU_ZERO(&target); CPU_SET(cpu, &target);
        if (!retained[i] && sched_setaffinity(worker->tid, sizeof(target), &target)) return -1;
        tids[i] = worker->tid;
        team->members[i] = worker;
        team->cpus[i] = cpu;
        team->current[i] = NULL;
        team->count++;
    }
    if (dt_members(client, tids, count)) return -1;
    team->registered = true;
    return 0;
}
