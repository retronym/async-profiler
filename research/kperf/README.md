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

## Prototype gaps

These are specific to `kperf_sample.c`. The spike below addresses the first three; the rest carry over into the consolidated list at the end.

- No inlined frames: `Compiler.codelist` gives nmethod ranges only.
- The arm64 fixup frame (LR at the interrupt) is dropped.
- Codelist snapshots every 2s can misattribute code that was unloaded and its address reused in between.

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

## asprof launcher on a real app

`sudo asprof -e cycles -d 30 -f out.html <pid>` against an idle sbt 2.0 server (JDK 21, `-XX:+PreserveFramePointer -XX:+DebugNonSafepoints`):

- `asprof` launched `asprof-kperf -p <pid>` for each session. The helper served the agent, then removed its socket and private `/tmp` directory, and exited.
- The stacks are plausible, mixing compiled, interpreted, lambda and stub frames. For example: `ThreadPoolExecutor.runWorker` → `Executors$RunnableAdapter.call` → `sbt/internal/TaskProgress$$Lambda…run` → `TaskProgress.$anonfun$schedule$1` → `JFunction0$mcV$sp.apply` → `itable stub`.
- sbt was idle, so there were only 29 samples in 10s at the default 1M-cycle period.

## Caveats

**Platform and privileges**

- **Root is required.** The PMU interfaces are private frameworks, and the entitlement that would avoid root is Apple-only. Function signatures and struct layouts can change in any macOS update.
- **One owner at a time.** The counters, kperf and the kernel trace buffer are global. Each new session starts by clearing the trace buffer, so it silently clobbers whatever else is using them: Instruments/xctrace, `ktrace`, or a second `asprof` / helper.
- **Tested only on an M5 Pro with macOS 27.** Intel Macs use the same API but are untested. The event aliases (`cache-misses` → `L1D_CACHE_MISS_LD_NONSPEC`, etc.) are Apple-silicon names. Counters don't work inside VMs.

**Stack quality**

- **`-XX:+PreserveFramePointer` is required,** and can't be turned on in a running JVM. The kernel walks frame pointers only; without the flag, compiled Java frames drop out of the stack. With JMH, the flag has to reach the benchmark fork (e.g. `-jvmArgsPrepend`), not just sbt's `.jvmopts`.
- **Interpreted frames are unnamed (`[interpreted]`).** The kernel records only return addresses, not frame pointers, so the method can't be recovered. This matters for JMH single-shot runs, where the stub and outer driver frames stay interpreted (and hide `jmh_generated` from filters).
- **Leaf frames can be off by a frame.** Frameless leaf functions depend on the LR heuristic, which isn't validated beyond these runs. The leaf PC is mapped to the nearest debug-info entry, so `-XX:+DebugNonSafepoints` helps. Interrupts have skid: a sample can land a few instructions after the event that caused it.
- **Stacks are capped at 256 frames** by the kernel, cut at the root end.

**Resolution in the agent**

- **Stale code.** A sample is resolved milliseconds after it's taken, and code can be unloaded or recompiled at the same address in between. There's crash protection, but no check that the method found is the one that was running.
- **The native-symbol cache never invalidates,** so it would give wrong names if a library were unloaded.
- **Timestamps are when the agent processes a sample,** not the kernel's interrupt time.
- **Thread-id mapping cost.** A sample from a thread not yet in the map re-lists every thread in the JVM. That's visible on apps that create threads often, like sbt.
- **The reader thread samples itself:** about 0.2% of misses and 0.1% of cycles, but proportionally large when the app is idle.

**Scope**

- **One event per session.**
- **Samples are recorded as `PERF_SAMPLE`,** so JFR doesn't carry the event name.
- **The interrupt fires on every process.** The pid filter is applied after the interrupt, so a small period costs kernel time across the whole system.
- **The one-shot helper (`-p <pid>`) gives up after 30s** if the agent hasn't connected.
- **No automated tests.** `run.sh` and `spike.sh` are manual, root-only integration tests.

## Precise PC capture

Apple silicon can record the exact instruction address that caused a counter event. This machine reports `kpc.pc_capture_supported: 1`. XNU's own test for it, `pmi_pc_capture` in `tests/cpu_counters/kpc_tests.c`, says it only works on configurable counters 5, 6 and 7. When it works, the interrupt's trace event (`PERF_KPC_HNDLR`) has the "PC captured" flag (`0x08` in `desc >> 48`) set and carries the exact PC.

Our runs reported 0 captured PCs because kpep put the event on counter 0. With a precise PC as the leaf frame, cache-miss and branch-miss profiles would point at the actual load or branch instead of a skidded one. This is presumably what Instruments' "L1D Miss Sampling" guided mode uses.

To get there, steer the event onto counters 5–7. The XNU test programs the same event on every configurable counter, then gives only counters 5–7 an interrupt period and a kperf action. There's one capture register, so it staggers periods across counters to stop them overwriting each other's PCs. Not every event may be allowed on those counters.

## Future work

1. **Precise PC capture**, as above. Pass the captured PC through the protocol and use it as the leaf frame.
2. **A helper exclusivity check.** Refuse, rather than clobber, when the counters or trace buffer are already in use.
3. **`benchLabAsync` hook** (scala3 bench-lab). Add `ASYNC_PROFILER_KPERF` support so JMH forks can use a long-running helper (`sudo asprof-kperf -s /tmp/asprof-kperf.sock` plus `-prof "async:libPath=…;event=…;rawCommand=kperf=/tmp/asprof-kperf.sock"`) without fighting sbt's `;` splitting.
4. **Multiple events per session.** Use one kperf action per counter; the interrupt's trace event already says which counter fired. Pairs like cycles with misses would give per-stack ratios.
5. **Use kernel timestamps,** converted to async-profiler's clock.
6. **Verify the method at resolve time,** e.g. check it is still live at the sample's timestamp, or track compiled-method load and unload events.
7. **Cheaper thread-id mapping.** Look up only the missing thread instead of re-listing all of them.
8. **Kernel stacks** (`KPERF_SAMPLER_KSTACK`): cheap to collect, but need kernel symbolication.
9. **An event type that records the event name** in JFR, if `PERF_SAMPLE` proves too lossy.
10. **Upstream hygiene:** automated tests gated on root and real hardware; docs in `CpuSamplingEngines.md`; signing and notarizing `asprof-kperf`; Intel Mac testing; a launchd-daemon option to avoid `sudo` per session.
