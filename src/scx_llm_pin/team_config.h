/* SPDX-License-Identifier: GPL-2.0 */
#ifndef LLM_TEAM_CONFIG_H
#define LLM_TEAM_CONFIG_H
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include "scx_llm_pin.h"

/* PID:decode_threads:prefill_threads[:cpu_mask], e.g. 123:4:8:0xff. */
static inline int llm_parse_team(const char *text, struct llm_team_config *out)
{
    unsigned long values[4] = {0, 0, 0, LLM_CPU_MASK};
    const char *p = text;
    for (unsigned i = 0; i < 4; i++) {
        char *end;
        if (!*p || *p == '-' || *p == '+' || *p == ' ') return -1;
        errno = 0;
        values[i] = strtoul(p, &end, i == 3 ? 0 : 10);
        if (errno || end == p || values[i] > 0x7fffffffUL) return -1;
        if (!*end) {
            if (i < 2) return -1;
            break;
        }
        if (*end != ':' || i == 3) return -1;
        p = end + 1;
    }
    unsigned mask = values[3];
    unsigned cores = __builtin_popcount(mask);
    if (!values[0] || !mask || (mask & ~LLM_CPU_MASK) ||
        !values[1] || values[1] > cores || !values[2] || values[2] > cores)
        return -1;
    memset(out, 0, sizeof(*out));
    out->tgid = values[0];
    out->threads = values[1];
    out->threads_batch = values[2];
    out->cpu_mask = mask;
    return 0;
}

/* Spread smaller teams across the board, preserving a unique order per team. */
static inline int llm_prepare_teams(struct llm_pin_config *cfg)
{
    unsigned load[RVV_LEASE_MAX_CPUS] = {0};
    if (!cfg->team_count || cfg->team_count > RVV_LEASE_CONTROL_TEAMS) return -1;
    for (unsigned t = 0; t < cfg->team_count; t++) {
        struct llm_team_config *team = &cfg->teams[t];
        unsigned mask = team->cpu_mask;
        unsigned cores = __builtin_popcount(mask);
        if (!team->tgid || !mask || (mask & ~LLM_CPU_MASK) ||
            !team->threads || team->threads > cores ||
            !team->threads_batch || team->threads_batch > cores) return -1;
        for (unsigned j = 0; j < t; j++)
            if (cfg->teams[j].tgid == team->tgid) return -1;
        unsigned width = team->threads > team->threads_batch ?
            team->threads : team->threads_batch;
        unsigned used = 0;
        for (unsigned i = 0; i < cores; i++) {
            unsigned best = RVV_LEASE_MAX_CPUS;
            for (unsigned cpu = 0; cpu < RVV_LEASE_MAX_CPUS; cpu++) {
                if (!(mask & (1U << cpu)) || (used & (1U << cpu))) continue;
                if (best == RVV_LEASE_MAX_CPUS || load[cpu] < load[best]) best = cpu;
            }
            team->cpu_order[i] = best;
            used |= 1U << best;
            if (i < width) load[best]++;
        }
    }
    cfg->target_tgid = cfg->teams[0].tgid;
    cfg->target_tgid2 = cfg->team_count > 1 ? cfg->teams[1].tgid : 0;
    return 0;
}
#endif
