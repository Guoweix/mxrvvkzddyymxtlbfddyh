/* SPDX-License-Identifier: MIT */
/* Small board-only hint producer for 8-team queue/lifecycle tests without GGUFs.
 * Build with -I/path/to/llama.cpp/ggml/src/ggml-cpu. This is NOT an LLM benchmark.
 */
#define GGML_USE_OPENMP
#include "scx-barrier-hint-client.c"
int main(int argc, char **argv)
{
    unsigned phase = argc > 1 ? (unsigned)strtoul(argv[1], NULL, 10) : RVV_LEASE_PHASE_DECODE;
    uint64_t end = monotonic_ns() + 30000000000ULL;
    while (monotonic_ns() < end) {
        ggml_scx_lease_hint_phase_begin(phase, phase == RVV_LEASE_PHASE_PREFILL ? 128 : 0, 1, 1);
        ggml_scx_lease_hint_graph_begin(1);
        ggml_scx_lease_hint_register((uint32_t)syscall(SYS_gettid), 0, 1);
        uint64_t until = monotonic_ns() + 20000000ULL;
        while (monotonic_ns() < until) {
#if defined(__riscv)
            asm volatile(".option push\n.option arch,+v\nvsetivli zero, 8, e32, m1, ta, ma\nvmv.v.i v0, 1\n.option pop" ::: "memory");
#endif
        }
        usleep(1000);
    }
    ggml_scx_lease_hint_phase_end();
    return 0;
}
