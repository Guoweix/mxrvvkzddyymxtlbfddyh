/* SPDX-License-Identifier: GPL-2.0 */
#define _GNU_SOURCE
#include "client.h"
#include <errno.h>
#include <inttypes.h>
#include <omp.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/syscall.h>
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
    if (argc != 4) { fprintf(stderr, "usage: %s socket iterations compute_us\n", argv[0]); return 2; }
    unsigned iterations = strtoul(argv[2], NULL, 10), compute_us = strtoul(argv[3], NULL, 10);
    if (!iterations || iterations > 100000 || compute_us > 1000000) return 2;
    struct dt_client c;
    omp_set_dynamic(0);
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (dt_connect(&c, argv[1], 8) || dt_activity(&c, 1)) { perror("connect/activity"); return 1; }
    puts("epoch,slot,workers,cpu_mask,pin,acquire_ns,total_ns");
    for (unsigned i = 0; i < iterations; i++) {
        uint64_t start = now_ns();
        if (dt_acquire(&c, 8)) { perror("acquire"); return 1; }
        uint64_t granted = now_ns();
        uint32_t tids[DT_WORKERS] = {0};
        int errors[DT_WORKERS] = {0};
        int members_error = 0, actual = 0;
        unsigned workers = c.grant.workers;
        #pragma omp parallel num_threads(workers) shared(actual, members_error)
        {
            unsigned ith = omp_get_thread_num();
            cpu_set_t original, target;
            int cpu = dt_cpu(&c, ith);
            tids[ith] = syscall(SYS_gettid);
            int have_original = !sched_getaffinity(0, sizeof(original), &original);
            if (!have_original) errors[ith] = 1;
            if (cpu >= 0) {
                CPU_ZERO(&target); CPU_SET(cpu, &target);
                if (!have_original || !CPU_ISSET(cpu, &original) ||
                    sched_setaffinity(0, sizeof(target), &target)) errors[ith] = 1;
            }
            #pragma omp barrier
            #pragma omp single
            {
                actual = omp_get_num_threads();
                for (unsigned j = 0; j < workers; j++) members_error |= errors[j];
                if (actual != (int)workers || members_error || dt_members(&c, tids, actual)) members_error = 1;
            }
            if (!members_error) {
                uint64_t until = now_ns() + (uint64_t)compute_us * 1000;
                do {
                    if (cpu >= 0 && sched_getcpu() != cpu) errors[ith] = 1;
                    /* Force real scheduling while checking all granted worker locations. */
                    sched_yield();
                } while (now_ns() < until);
            }
            #pragma omp barrier
            #pragma omp single
            {
                if (!members_error && dt_unmembers(&c)) members_error = 1;
            }
            if (have_original && sched_setaffinity(0, sizeof(original), &original)) errors[ith] = 1;
        }
        for (unsigned j = 0; j < workers; j++) members_error |= errors[j];
        if (members_error) { fprintf(stderr, "FAIL workers/placement epoch=%" PRIu64 "\n", c.epoch); return 1; }
        if (dt_release(&c)) { perror("release"); return 1; }
        printf("%" PRIu64 ",%u,%u,%" PRIu64 ",%u,%" PRIu64 ",%" PRIu64 "\n",
               c.epoch, c.slot, workers, c.grant.cpu_mask, c.grant.pin, granted - start, now_ns() - start);
    }
    if (dt_activity(&c, 0)) { perror("idle"); return 1; }
    dt_close(&c);
    printf("PASS iterations=%u\n", iterations);
    return 0;
}
