/* Real OpenMP + affinity syscalls; only the protocol endpoint is stubbed.
 * Count actual sched_setaffinity calls, including redundant singleton pins. */
#define _GNU_SOURCE
#include "worker_team.h"
#include <assert.h>
#include <omp.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

static struct { pid_t tid; unsigned calls; } calls[256];
static unsigned ncalls;
static bool published;
static struct dt_team team;
static struct dt_client client;
static pid_t observed[DT_WORKERS];
static unsigned nshrinks;

int __real_sched_setaffinity(pid_t tid, size_t size, const cpu_set_t *mask);
int __wrap_sched_setaffinity(pid_t tid, size_t size, const cpu_set_t *mask)
{
    if (!tid) tid = syscall(SYS_gettid);
    unsigned i;
    for (i = 0; i < ncalls && calls[i].tid != tid; i++);
    if (i == ncalls) { assert(ncalls < 256); calls[ncalls++].tid = tid; }
    calls[i].calls++;
    return __real_sched_setaffinity(tid, size, mask);
}
static unsigned count_calls(pid_t tid)
{
    for (unsigned i = 0; i < ncalls; i++) if (calls[i].tid == tid) return calls[i].calls;
    return 0;
}
int dt_cpu(const struct dt_client *c, unsigned ith)
{
    for (int cpu = 0; cpu < DT_WORKERS; cpu++)
        if ((c->grant.cpu_mask & (1ULL << cpu)) && !ith--) return cpu;
    return -1;
}
int dt_members(struct dt_client *c, const uint32_t *tids, unsigned count)
{
    assert(c->held && !published);
    for (unsigned i = 0; i < count; i++) {
        cpu_set_t mask;
        assert(!sched_getaffinity(tids[i], sizeof(mask), &mask));
        assert(CPU_COUNT(&mask) == 1 && CPU_ISSET(dt_cpu(c, i), &mask));
    }
    published = true;
    return 0;
}
int dt_unmembers(struct dt_client *c) { assert(c->held && published); published = false; return 0; }
int dt_release(struct dt_client *c) { assert(c->held && !published); c->held = false; return 0; }
int dt_shrink(struct dt_client *c)
{
    assert(c->held && c->revoked && !published);
    c->grant.cpu_mask = c->revoke.cpu_mask;
    c->grant.workers = c->revoke.workers;
    c->revoked = false;
    nshrinks++;
    return 0;
}
static void graph(unsigned width)
{
    #pragma omp parallel num_threads(width)
    {
        unsigned ith = omp_get_thread_num();
        observed[ith] = syscall(SYS_gettid);
        assert(!dt_team_observe(&team, ith));
        #pragma omp barrier
        #pragma omp single
        { assert(!dt_team_commit(&team, &client, width)); }
        assert(sched_getcpu() == dt_cpu(&client, ith));
    }
}
static void shrink(unsigned width, uint64_t mask)
{
    pid_t old[DT_WORKERS];
    unsigned before[DT_WORKERS];
    memcpy(old, observed, sizeof(old));
    for (unsigned i = 0; i < DT_WORKERS; i++) before[i] = count_calls(old[i]);
    unsigned old_width = client.grant.workers;
    client.revoked = true;
    client.revoke.workers = width;
    client.revoke.cpu_mask = mask;
    assert(!dt_team_revoke(&team, &client));
    /* Check BEFORE OpenMP can retire surplus threads. Removed workers alone
       must have their original allowed mask restored before partial return. */
    for (unsigned i = 0; i < old_width; i++) {
        cpu_set_t affinity;
        assert(!sched_getaffinity(old[i], sizeof(affinity), &affinity));
        assert(count_calls(old[i]) == before[i] + (i >= width));
        if (i < width) assert(CPU_COUNT(&affinity) == 1 && CPU_ISSET(dt_cpu(&client, i), &affinity));
        else assert(CPU_EQUAL(&affinity, &team.allowed));
    }
    graph(width);
    for (unsigned i = 0; i < width; i++) {
        assert(observed[i] == old[i]);
        assert(count_calls(old[i]) == before[i]);
    }
    printf("PASS shrink %u -> %u: surviving TIDs/CPUs unchanged, zero affinity calls\n", old_width, width);
}
static void detach(void)
{
    assert(!dt_team_detach(&team, &client));
    cpu_set_t affinity;
    assert(!sched_getaffinity(0, sizeof(affinity), &affinity));
    assert(CPU_EQUAL(&affinity, &team.allowed));
}
int main(void)
{
    omp_set_dynamic(0);
    assert(!dt_team_init(&team));
    for (int cpu = 0; cpu < DT_WORKERS; cpu++) {
        if (!CPU_ISSET(cpu, &team.allowed)) { puts("SKIP requires CPU0-7"); return 77; }
    }
    client.held = true;
    client.grant.pin = 1;
    client.grant.workers = 8;
    client.grant.cpu_mask = 255;
    graph(8);
    shrink(4, 15);
    shrink(1, 1);
    /* New workers can inherit a still-pinned master after partial return.
       Check the widened baseline, then test the actual 7 -> 4 phase case. */
    detach();
    client.grant.workers = 7;
    client.grant.cpu_mask = 254;
    graph(7);
    shrink(4, 30);
    /* Width-only change within a grant also retains surviving bindings. */
    pid_t master = observed[0];
    unsigned before = count_calls(master);
    graph(1);
    assert(count_calls(master) == before);
    graph(4);
    assert(count_calls(master) == before);
    detach();
    /* Force TLS retirement/recreation; dead worker identities are not reused. */
    assert(!omp_pause_resource_all(omp_pause_hard));
    graph(4);
    detach();
    assert(nshrinks == 3);
    puts("PASS width changes, worker recreation, final affinity restoration");
    return 0;
}
