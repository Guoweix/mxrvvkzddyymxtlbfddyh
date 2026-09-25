/* SPDX-License-Identifier: GPL-2.0 */
/* Minimal affinity launcher for Buildroot boards without util-linux taskset. */
#define _GNU_SOURCE
#include <errno.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "scx_rvv_lease_control.h"
int main(int argc, char **argv)
{
    char *end;
    if (argc < 3) {
        fprintf(stderr, "Usage: %s CPU_MASK COMMAND [ARGS...]\n", argv[0]);
        return 2;
    }
    errno = 0;
    unsigned long mask = strtoul(argv[1], &end, 0);
    if (errno || end == argv[1] || *end || !mask || mask > 255) {
        fprintf(stderr, "CPU mask must be in 1..0xff\n"); return 2;
    }
    cpu_set_t allowed, selected;
    if (sched_getaffinity(0, sizeof(allowed), &allowed)) {
        perror("sched_getaffinity"); return 1;
    }
    CPU_ZERO(&selected);
    for (unsigned cpu = 0; cpu < RVV_LEASE_MAX_CPUS; cpu++) {
        if (!(mask & (1UL << cpu))) continue;
        if (!CPU_ISSET(cpu, &allowed)) {
            fprintf(stderr, "CPU %u is unavailable\n", cpu); return 2;
        }
        CPU_SET(cpu, &selected);
    }
    if (sched_setaffinity(0, sizeof(selected), &selected)) {
        perror("sched_setaffinity"); return 1;
    }
    execvp(argv[2], argv + 2);
    perror("execvp"); return 1;
}
