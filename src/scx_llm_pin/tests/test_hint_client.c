/* SPDX-License-Identifier: MIT */
/* Build with -I/path/to/llama.cpp/ggml/src/ggml-cpu. No BPF or board required. */
#define GGML_USE_OPENMP
#include "scx-barrier-hint-client.c"
#include <assert.h>
int main(void)
{
    static struct rvv_lease_worker_page page;
    page.header.active = 1;
    page.header.target_tgid = 100;
    page.header.worker_limit = 8;
    page.header.session_id = 123;
    g_lease_client.page = &page;
    g_lease_client.tgid = 100;
    g_lease_client.configured = true;
    strcpy(g_lease_client.path, "/test");
    ggml_scx_lease_hint_phase_begin(RVV_LEASE_PHASE_PREFILL, 128, 0, 8);
    ggml_scx_lease_hint_graph_begin(8);
    for (unsigned i=0; i<8; i++) ggml_scx_lease_hint_register(100+i, i, 8);
    uint64_t old = page.team_hint.membership_epoch;
    assert(old && page.workers[7].membership_epoch == old);
    ggml_scx_lease_hint_phase_end();
    ggml_scx_lease_hint_phase_begin(RVV_LEASE_PHASE_DECODE, 0, 1, 4);
    ggml_scx_lease_hint_graph_begin(4);
    for (unsigned i=0; i<4; i++) ggml_scx_lease_hint_register(200+i, i, 4);
    assert(page.team_hint.active_workers == 4);
    assert(page.team_hint.membership_epoch > old);
    assert(page.workers[3].tid == 203);
    assert(page.workers[3].membership_epoch == page.team_hint.membership_epoch);
    assert(page.workers[7].membership_epoch != page.team_hint.membership_epoch);
    ggml_scx_lease_hint_graph_begin(1);
    ggml_scx_lease_hint_register(300, 0, 1);
    assert(page.workers[0].tid == 300 && page.workers[0].n_threads == 1);
    ggml_scx_lease_hint_graph_begin(9);
    ggml_scx_lease_hint_register(999, 0, 9);
    assert(page.workers[0].tid == 300 && page.team_hint.expires_ns == 0);
    assert(!(page.team_hint.sequence & 1));
    puts("hint membership tests passed");
}
