#include "allocation.h"
#include <assert.h>
#include <stdio.h>

static void check(const struct dt_demand *in, struct dt_target *out, size_t n, uint64_t mask)
{
    dt_allocate(in, out, n, mask);
    uint64_t used = 0;
    for (size_t i = 0; i < n; i++) {
        assert(!(out[i].mask & ~mask) && !(out[i].mask & used));
        assert(out[i].workers <= in[i].limit);
        assert(__builtin_popcountll(out[i].mask) == (int)out[i].workers);
        used |= out[i].mask;
    }
}
int main(void)
{
    struct dt_demand in[8] = {{.eligible = true, .limit = 8}};
    struct dt_target out[8];
    check(in, out, 1, 255);
    assert(out[0].workers == 8 && out[0].mask == 255);
    in[0].retained_mask = 255;
    in[1] = (struct dt_demand){.eligible = true, .limit = 8};
    check(in, out, 2, 255);
    assert(out[0].mask == 15 && out[1].mask == 240);
    in[0].retained_mask = 0;
    in[1].retained_mask = 15;
    check(in, out, 2, 255);
    assert(out[0].mask == 240 && out[1].mask == 15);
    in[0].limit = 2;
    check(in, out, 2, 255);
    assert(out[0].workers == 2 && out[1].workers == 6);
    in[1].priority = 1;
    check(in, out, 2, 255);
    assert(!out[0].workers && out[1].workers == 8);
    check(in, out, 2, 240);
    assert(out[1].mask == 240);
    for (unsigned i = 0; i < 8; i++) in[i] = (struct dt_demand){.eligible = true, .limit = 8};
    check(in, out, 3, 255);
    assert(out[0].workers == 3 && out[1].workers == 3 && out[2].workers == 2);
    check(in, out, 8, 255);
    for (unsigned i = 0; i < 8; i++) assert(out[i].workers == 1);
    check(in, out, 8, 0);
    for (unsigned i = 0; i < 8; i++) assert(!out[i].workers);
    /* One prefill, then 1/3/7 protected decodes. */
    in[0].priority = 1;
    for (unsigned i = 1; i < 8; i++) in[i].minimum = 1;
    const unsigned counts[] = {2, 4, 8};
    for (unsigned k = 0; k < 3; k++) {
        unsigned n = counts[k];
        check(in, out, n, 255);
        assert(out[0].workers == 9 - n);
        for (unsigned i = 1; i < n; i++) assert(out[i].workers == 1);
    }
    in[1].priority = 1;
    in[1].minimum = 0;
    check(in, out, 4, 255);
    assert(out[0].workers == 3 && out[1].workers == 3);
    assert(out[2].workers == 1 && out[3].workers == 1);
    /* No prefill: an existing floor must not bias equal sharing. */
    in[0].priority = in[1].priority = 0;
    in[1].minimum = 1;
    check(in, out, 2, 255);
    assert(out[0].workers == 4 && out[1].workers == 4);
    check(in, out, 3, 255);
    assert(out[0].workers == 3 && out[1].workers == 3 && out[2].workers == 2);
    /* Minimal migration from 4/4 -> protected decode 1/prefill 7. */
    in[0].minimum = 1;
    in[0].retained_mask = 15;
    in[1].priority = 1;
    in[1].retained_mask = 240;
    check(in, out, 2, 255);
    assert(out[0].mask == 1 && out[1].mask == 254);
    check(in, out, 8, 240);  // quarantined low CPUs, insufficient for all floors
    check(in, out, 8, 0);
    in[0].eligible = false;
    in[1].limit = 0;
    check(in, out, 8, 255);
    assert(!out[0].workers && !out[1].workers);
    puts("PASS allocation: 1/2/3/8 peers, floors, equal sharing, caps, priorities, retention, unavailable CPUs");
}
