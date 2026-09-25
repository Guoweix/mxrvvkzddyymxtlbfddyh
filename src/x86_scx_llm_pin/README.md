# x86_scx_llm_pin

Self-contained x86_64 port of the RISC-V `scx_llm_pin` sched_ext scheduler,
with SMT (hyperthreading) support for the Intel i5-10200H (4 physical cores /
8 logical CPUs, sibling pairs `0/4, 1/5, 2/6, 3/7`).

Everything needed to build is in this directory. There is no `-I../scx_llm_pin`,
no reference to any sibling repository path, and no cross toolchain.

## Files

| File | Purpose |
| --- | --- |
| `scx_llm_pin.bpf.c` | BPF scheduler. RISC-V `sstatus.VS` path under `#ifndef SCX_ARCH_X86`; ABI v4 worker-hint classification and x86 SMT placement under `#ifdef SCX_ARCH_X86`. |
| `scx_llm_pin.c` | User-space loader, config validation, spread topology check, stats. |
| `scx_llm_pin.h` | Shared ABI/config/stats definitions (`rvv_*` names preserved). |
| `scx_rvv_lease_control.h` | ABI v4 worker page (unchanged, shared with the patched llama runtime). |
| `team_config.h` | `--team PID:T:TB[:MASK]` parser and unique-CPU planner. |
| `Makefile` | Generates `vmlinux.h` from the running kernel BTF and builds `x86_scx_llm`. |
| `tools/bpftool` | Bundled x86_64 `bpftool` fallback (PATH version is preferred). |
| `tests/test_team_config.c` | Host-only unit test for team parsing (no root/BPF). |

## Host requirements

* `clang` with the BPF target (`-target bpf`)
* `cc` (host gcc/clang)
* `libbpf` development files (`pkg-config --libs libbpf`)
* `bpftool` **from `PATH`**, otherwise the bundled `tools/bpftool`
* read access to `/sys/kernel/btf/vmlinux` (the running kernel's BTF)

`bpftool` is used to generate `vmlinux.h` and the BPF skeleton. The Makefile
prefers `bpftool` from `PATH`; if it is absent it falls back to the bundled
copy and, only if neither exists, fails with a clear error.

## Build and test

```sh
make            # -> x86_scx_llm
make test       # no sched_ext/root: --help, --check-config, skeleton gen
make clean
```

`make test` covers the two-team config, rejection of invalid spread configs
(non-`0xff` mask, more than two teams), `bpftool gen skeleton`, and the
team-config unit test.

## Usage

The scheduler is attached by the loader itself; `make test` never loads it.
x86 placement is fixed to spread; there is no placement flag.

```sh
# Two llama-server models, 4 workers each (one per physical core):
sudo ./x86_scx_llm --team <PID0>:4:4:0xff --team <PID1>:4:4:0xff
```

Per server the recommended llama.cpp settings are `-t 4 -tb 4`, i.e. four
workers mapped one-per-physical-core.

Useful options:

```
--team PID:T:TB[:MASK]       repeat for 1..8 models (spread requires exactly 2)
--vector-slice-ms <ms>       vector worker slice (default 100)
--vector-local-window-ms <ms> local fast-path window (default 500)
--phase-hint-timeout-ms <ms> reject stale phase hints (default 300000)
--prefill-starvation-ms <ms> prefill/MIXED safety wait (default 1000)
--phase-wakeup-preempt on|off targeted wakeup preemption (default on)
--tgid/--tgid2               legacy single/double model form
--check-config               validate + print, do not load
```

## What is preserved from the RISC-V behavior

* Single strategy (`rvv_dirty_local`): classify LLM workers, give each a unique
  CPU, per-team/per-CPU vector DSQs, non-vector LLM tasks to the local DSQ.
* Automatic phase-aware consumption: valid ABI v4 phase hints make PREFILL /
  MIXED teams preferred in per-CPU vector-DSQ consumption, with targeted wakeup
  preemption and the prefill safety slice; without valid hints it falls back to
  plain round-robin.
* The 500 ms local window (`vector_local_window_ns > 0`): after a vector DSQ is
  consumed the team's vector tasks go to the local DSQ for the window.
* `scx_bpf_kick_cpu(cpu, 0)` when a vector DSQ transitions empty -> non-empty
  and the target CPU differs from the current one.
* All ABI v4 worker page / config map plumbing and the `rvv_*` struct names, so
  the same patched llama runtime works.

## x86 classification (VS -> hint)

x86 has no architectural `sstatus.VS` dirty bit, so `read_vs_state()` is stubbed
to `ok = false` under `SCX_ARCH_X86`, and worker classification instead reads
the ABI v4 worker map (`user_worker_map`, `rvv_lease_worker_slot`) recovered
from the original implementation:

* `worker_slot_matches()` verifies a coherent slot (`sequence`, `tid`, `tgid`,
  `ith`, `n_threads`, `session_id`, `membership_epoch`).
* `apply_user_worker_hint()` returns member (`1`), complete non-member (`2`),
  incomplete (`0`), unavailable (`-1`) or invalid count (`-2`). A member is a
  vector worker, a complete non-member/invalid count is demoted, and an
  incomplete generation keeps using the ordinary scheduler.
* Because the runtime publishes its worker slot after a worker has already been
  woken, `stopping` re-samples the ABI on x86 so a preempted worker is placed on
  its team DSQ by the next enqueue.

The RISC-V path (`#ifndef SCX_ARCH_X86`) is untouched and still uses
`bpf_task_pt_regs()` + `RVV_SR_VS_DIRTY` with the dirty-score threshold.

## SMT placement (fixed spread)

`select_x86_smt_cpu()` always maps the ABI worker slot index `ith` (0..3, one per
physical core) with a fixed team split, independent of phase:

| Team | CPUs |
| --- | --- |
| team0 | `0..3` |
| team1 | `4..7` |

For a 2×4 setup on 4C/8T this fills all eight logical CPUs (one worker of each
team per physical core). The earlier phase-dependent `adaptive` policy and the
`--x86-smt` flag were removed after measurement showed fixed spread is faster
for the non-oversubscribed 8-thread case.

Spread is validated at startup: x86 build, exactly two teams, `T/TB <= 4`,
CPU mask `0xff`, and a live 4C/8T topology check
(`/sys/devices/system/cpu/cpu*/topology/{physical_package_id,core_id}` must show
sibling pairs `0/4, 1/5, 2/6, 3/7`).

`x86_smt_primary_selections` / `x86_smt_sibling_selections` are printed with the
rest of the stats.

## Differences from the original x86 refactor

The current RISC-V tree is a simplified, single-strategy refactor. The following
original modes were intentionally not carried over and are documented rather
than silently dropped:

* `--strategy`, `--user-control`, `--user-cpu-map`, `--team-gang`,
  `--gang-quantum-us`, `--gang-sample-us`, `--vector-local-wakeup` and
  `--control-dir`: the current design is always hint-classified, always
  phase-aware and always uses the default control directory.
* `pair-gang` (`LLM_TEAM_GANG_PAIR`): removed with `--team-gang`, and along with
  it the x86 placement flag. `select_x86_smt_cpu` contains only the fixed spread
  path; the pair-gang stats are not present.
* The original `user_hint_*`, `gang_*` and `protected_kernel_*` stats were not
  reintroduced. The ABI/stat field names that do exist (`rvv_*`) are unchanged.

## Notes on the bundled bpftool

`tools/bpftool` is an x86_64 copy kept only as a convenience fallback so the
tree is buildable on hosts without `bpftool` installed. `PATH` always wins; the
bundled binary is never referenced from the source or from another directory.
