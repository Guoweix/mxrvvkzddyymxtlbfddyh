/* SPDX-License-Identifier: GPL-2.0 */
#define _GNU_SOURCE
#include "worker_team.h"
#include <errno.h>
#include <inttypes.h>
#include <omp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}
int main(int argc, char **argv)
{
    if (argc != 5) {
        fprintf(stderr, "usage: %s socket iterations compute_us stable|vary|recreate\n", argv[0]); return 2;
    }
    unsigned iterations = strtoul(argv[2], NULL, 10), compute_us = strtoul(argv[3], NULL, 10);
    if (!iterations || iterations > 100000 || compute_us > 45000000 ||
        (strcmp(argv[4], "stable") && strcmp(argv[4], "vary") && strcmp(argv[4], "recreate"))) return 2;
    struct dt_client c;
    struct dt_team team;
    omp_set_dynamic(0);
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (dt_team_init(&team) || dt_connect(&c, argv[1], 8) ||
        c.lifetime != DT_PERSISTENT || dt_activity(&c, 1)) goto fail;
    for (unsigned graph = 0; graph < iterations; graph++) {
        if (dt_poll(&c)) goto fail;
        if (c.revoked && dt_team_revoke(&team, &c)) goto fail;
        if (!c.held && !c.pending && dt_request(&c, 8)) goto fail;
        if (dt_wait_grant(&c)) goto fail;
        while (c.revoked) {
            if (dt_team_revoke(&team, &c) || dt_wait_grant(&c)) goto fail;
        }
        const unsigned widths[] = {8, 4, 2, 1, 8};
        unsigned workers = c.grant.workers;
        if (!strcmp(argv[4], "vary") && workers > widths[graph % 5]) workers = widths[graph % 5];
        int errors[DT_WORKERS] = {0}, failed = 0;
        #pragma omp parallel num_threads(workers) shared(failed)
        {
            unsigned ith = omp_get_thread_num();
            errors[ith] = dt_team_observe(&team, ith) != 0;
            #pragma omp barrier
            #pragma omp single
            {
                for (unsigned i = 0; i < workers; i++) failed |= errors[i];
                if (omp_get_num_threads() != (int)workers || failed || dt_team_commit(&team, &c, workers))
                    failed = 1;
            }
            if (!failed) {
                uint64_t until = now_ns() + (uint64_t)compute_us * 1000;
                do {
                    if (sched_getcpu() != dt_cpu(&c, ith)) errors[ith] = 1;
                    sched_yield();
                } while (now_ns() < until);
            }
        }
        for (unsigned i = 0; i < workers; i++) failed |= errors[i];
        if (failed) goto fail;
        printf("GRAPH %u epoch=%" PRIu64 " workers=%u mask=%" PRIu64 "\n",
               graph, c.epoch, workers, c.grant.cpu_mask);
        if (dt_poll(&c)) goto fail;
        if (c.revoked && dt_team_revoke(&team, &c)) goto fail;
        if (!strcmp(argv[4], "recreate") && omp_pause_resource_all(omp_pause_hard)) goto fail;
    }
    if (dt_activity(&c, 0) || (c.held && (dt_team_detach(&team, &c) || dt_release(&c)))) goto fail;
    cpu_set_t restored;
    if (sched_getaffinity(0, sizeof(restored), &restored) || !CPU_EQUAL(&restored, &team.allowed)) goto fail;
    dt_close(&c);
    printf("PASS iterations=%u\n", iterations);
    return 0;
fail:
    perror("persistent probe failed (no fallback)");
    return 1;
}
