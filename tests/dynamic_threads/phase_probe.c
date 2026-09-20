/* Exercise the exact llama runtime adapter with real persistent OpenMP teams. */
#define _GNU_SOURCE
#include "dynamic-grant-client.h"
#include <omp.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static unsigned long long now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}
int main(int argc, char **argv)
{
    if (argc != 5) return 2;
    unsigned graphs = strtoul(argv[2], NULL, 10), prefill = strtoul(argv[3], NULL, 10);
    unsigned us = strtoul(argv[4], NULL, 10);
    if (!graphs || graphs > 100000 || prefill > graphs || us > 45000000) return 2;
    setenv("GGML_DYNAMIC_THREADS_SOCKET", argv[1], 1);
    setvbuf(stdout, NULL, _IOLBF, 0);
    omp_set_dynamic(0);
    cpu_set_t before, after;
    if (sched_getaffinity(0, sizeof(before), &before) || !ggml_dynamic_threads_set_active(true)) return 1;
    for (unsigned g = 0; g < graphs; g++) {
        unsigned phase = g < prefill ? 1 : 2, workers = 8;
        if (ggml_dynamic_threads_begin(8, &workers, phase, phase == 1 ? g + 1 : 0,
                                       phase == 2 ? 1 : 0) != 1) return 1;
        int failed = 0;
        #pragma omp parallel num_threads(workers) shared(failed)
        {
            unsigned ith = omp_get_thread_num();
            if (ggml_dynamic_threads_observe(ith)) {
                #pragma omp atomic write
                failed = 1;
            }
            #pragma omp barrier
            #pragma omp single
            {
                if (omp_get_num_threads() != (int)workers ||
                    ggml_dynamic_threads_register(NULL, workers)) failed = 1;
            }
            unsigned long long until = now_ns() + (unsigned long long)us * 1000;
            do {
                if (sched_getcpu() != ggml_dynamic_threads_cpu(ith)) {
                    #pragma omp atomic write
                    failed = 1;
                }
            } while (now_ns() < until);
        }
        unsigned mask = 0;
        for (unsigned ith = 0; ith < workers; ith++) mask |= 1u << ggml_dynamic_threads_cpu(ith);
        if (failed || ggml_dynamic_threads_end()) return 1;
        printf("GRAPH %u phase=%u workers=%u mask=%u\n", g, phase, workers, mask);
    }
    if (!ggml_dynamic_threads_set_active(false) || sched_getaffinity(0, sizeof(after), &after) ||
        !CPU_EQUAL(&before, &after)) return 1;
    printf("PASS iterations=%u phase runtime and restored affinity\n", graphs);
    return 0;
}
