/* SPDX-License-Identifier: GPL-2.0 */
#include "allocation.h"

void dt_allocate(const struct dt_demand *in, struct dt_target *out, size_t count,
                 uint64_t available)
{
    unsigned priority = 0, left = __builtin_popcountll(available);
    for (size_t i = 0; i < count; i++) {
        out[i] = (struct dt_target){0};
        if (in[i].eligible && in[i].limit && in[i].priority > priority)
            priority = in[i].priority;
    }
    /* Floors precede priority. Never allocate quarantined/unavailable CPUs. */
    for (size_t i = 0; i < count && left; i++) {
        if (!in[i].eligible) continue;
        unsigned floor = in[i].minimum < in[i].limit ? in[i].minimum : in[i].limit;
        out[i].workers = floor < left ? floor : left;
        left -= out[i].workers;
    }
    /* Fill the smallest highest-priority team first, including its floor.
       Otherwise a protected decode would unfairly receive 5/3 in all-decode. */
    while (left) {
        size_t first = count;
        for (size_t i = 0; i < count; i++) {
            if (!in[i].eligible || in[i].priority != priority ||
                out[i].workers >= in[i].limit) continue;
            if (first == count || out[i].workers < out[first].workers) first = i;
        }
        if (first == count) break;
        out[first].workers++;
        left--;
    }
    /* Keep existing CPUs before filling any instance from the free set. */
    uint64_t free = available;
    for (unsigned pass = 0; pass < 2; pass++) {
        for (size_t i = 0; i < count; i++) {
            uint64_t candidates = free & (pass ? available : in[i].retained_mask);
            while (candidates && __builtin_popcountll(out[i].mask) < (int)out[i].workers) {
                uint64_t cpu = candidates & -candidates;
                out[i].mask |= cpu;
                free &= ~cpu;
                candidates &= ~cpu;
            }
        }
    }
}
