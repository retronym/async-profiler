# kperf PMI sampling prototype (macOS)

## Problem

async-profiler has no hardware-counter events on macOS: there is no `perf_events`, and the PMU is only reachable via private frameworks (async-profiler#182). Instruments/xctrace can sample on counter overflow, but it cannot symbolize JIT frames and its output is awkward to consume. JMH's `XCTraceAsmProfiler` only attributes leaf PCs, not stacks.

## Approach

Drive the same kernel machinery Instruments uses, directly:

- `kperfdata` (`kpep_*`) turns an event name from `/usr/share/kpep` into a PMU config word.
- `kpc_set_config` / `kpc_set_period` program a configurable counter to overflow every N events; `kpc_set_actionid` binds the PMI to kperf action 1.
- Action 1 samples the user stack (`KPERF_SAMPLER_USTACK`), filtered by pid. XNU pends this to an AST and walks frame pointers on return to user, emitting `PERF_CS_UHDR` + `PERF_CS_UDATA` kdebug events tagged with the tid.
- We read kdebug directly (`sysctl KERN_KDREADTR`); no ktrace/Instruments involved.
- JIT frames are named from `jcmd Compiler.codelist` snapshots; native frames via `atos -p`.

Reference: XNU `osfmk/kperf/{action,callstack}.c`, `osfmk/kern/kpc_common.c`, `tests/cpu_counters/kpc_tests.c` (`kpc_pmi_configurable`).

## Usage

```bash
clang -O2 -o kperf_sample kperf_sample.c
./kperf_sample -l                                   # list events (no root needed)
sudo ./kperf_sample -p <pid> -e L1D_CACHE_MISS_LD_NONSPEC -c 20000 -d 10 -o out.collapsed
```

The JVM must run with `-XX:+PreserveFramePointer`. `run.sh` is the manual integration test against `Workload.java`.

## Results (M5 Pro, macOS 27.0.1, Temurin 21)

Full stacks from `thread_start` through libjvm into compiled Java frames. Share of samples by leaf method:

| leaf | `CORE_ACTIVE_CYCLE` | `INST_ALL` | `L1D_CACHE_MISS_LD_NONSPEC` |
|---|---|---|---|
| `chase` (random pointer chase) | 79% | 4% | 57% |
| `mix` (ALU only) | 15% | 36% | ~0% |
| `scan` (sequential) | 6% | 60% | 43% |

Miss counts for `chase` and `scan` are ≈1:1, as expected (2M dependent misses vs 256MB / 128B lines per round), while `chase` costs ~14× the cycles.

## Known gaps

- No inlined frames: `Compiler.codelist` gives nmethod ranges only, so samples land on the outermost compiled method.
- Interpreted frames and stubs (e.g. call stub + interpreted `main`) are `[unknown]`: the kernel records return addresses only, not FPs, so the `Method*` cannot be recovered.
- The arm64 "fixup" frame (LR at the interrupt) is dropped; frameless leaves will lose their caller.
- `pc captured` is 0: precise PC capture appears limited to PMCs 5–7 (see XNU `pmi_pc_capture` test); we were allocated PMC 0.
- Root only; kperf/kdebug are single-owner, so this conflicts with a concurrent Instruments/ktrace session.
- Codelist snapshots every 2s can misattribute code that was unloaded and its address reused in between.

## Future work

- Stream raw stacks from a root helper to the async-profiler agent in the target JVM and resolve PCs online against the live CodeCache, using async-profiler's existing nmethod/ScopeDesc support to recover inlined frames. Emit JFR so the existing converters and flame graphs work.
- Use the fixup LR when the PC is in a frameless JIT leaf.
- Steer the event onto PMC 5–7 to get precise PC capture for the leaf frame.
- Multiple events per run (one action per counter; the PMI event carries the counter index).

## Spike: helper + agent engine (see DESIGN.md)

`build/bin/asprof-kperf` (src/kperf/kperfHelper.c) plus the `KperfEvents` engine (src/kperfEvents.cpp) and `StackWalker::resolveRaw`. Manual integration test: `research/kperf/spike.sh`, which starts the helper under sudo and runs `Workload` with `-agentpath:...=start,event=<ev>,kperf=/tmp/asprof-kperf.sock`.

Results (agent started at JVM launch, so setup is included):

| leaf | `cycles` | `instructions` | `cache-misses` |
|---|---|---|---|
| `step` (inlined into `chase`) + `chase` | 68% | ~3% | 42% |
| `mix` | 13% | 27% | — |
| `scan` | 5% | 46% | 32% |

- Inlined frames resolve: the top miss stack is `…call_stub;[interpreted];Workload.chase;Workload.step`.
- The reader thread samples itself. The first cut cost 15% of L1D misses, because `findNativeMethod` scans every loaded library per frame. A direct-mapped native PC cache brought it to 0.19% (0.08% of cycles).
- One helper served three successive JVMs.
