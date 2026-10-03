/* SPDX-License-Identifier: GPL-2.0 */
#ifndef DT_ALLOCATION_H
#define DT_ALLOCATION_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Stable array order is the tie-breaker; no runtime team-count dependency. */
struct dt_demand {
    bool eligible;
    unsigned priority, limit, minimum;
    uint64_t retained_mask;
};
struct dt_target { unsigned workers; uint64_t mask; };
void dt_allocate(const struct dt_demand *in, struct dt_target *out, size_t count,
                 uint64_t available);
#endif
