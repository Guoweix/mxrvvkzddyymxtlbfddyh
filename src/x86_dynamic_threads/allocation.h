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

/* x86 SMT 感知分配：物理核 lane = 0x0f，sibling lane = 0xf0。
 * - 任一 team 处于 prefill：所有活跃 team 都用 0x0f（避免 sibling AVX2 竞争，
 *   由相位 lease 优先 prefill）；
 * - 全部 decode：按 slot 顺序交替 0x0f / 0xf0（启用 SMT lane）。
 * 每 team worker 上限为 min(limit, 4)。 */
void dt_allocate_smt(const struct dt_demand *in, struct dt_target *out, size_t count,
                     uint64_t available, bool any_prefill);
#endif
