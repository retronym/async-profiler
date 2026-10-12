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
- **One owner at a time.** The counters, kperf and the kernel trace buffer are global. The kernel arbitrates: a live foreground ktrace owner makes others fail with `EBUSY`, and forced counters held by another task give `EACCES`. The helper checks this up front and names the owner. A *background* tool (`tailspind`) is taken over, as Instruments does; it is notified when we reset. Counter config writes only need root, so another root process could still reprogram counters mid-session.
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

- **Collapsed/HTML output has no per-sample event field,** so with several events each stack gets a root pseudo-frame (`cycles`, …). JFR output uses `profiler.HardwareSample` instead (below).
- **The interrupt fires on every process.** The pid filter is applied after the interrupt, so a small period costs kernel time across the whole system.
- **The one-shot helper (`-p <pid>`) gives up after 30s** if the agent hasn't connected.
- **No automated tests.** `run.sh` and `spike.sh` are manual, root-only integration tests.

## Precise PC capture

Apple silicon can record the exact instruction address that caused a counter event. This machine reports `kpc.pc_capture_supported: 1`. XNU's arm64 PMI handler (`PMC_SUPPORTS_PC_CAPTURE` in `osfmk/arm64/kpc.c`) only reads the capture register for absolute PMCs 5–7, and only uses it if the register says it latched that PMC. The PC then arrives in `PERF_KPC_HNDLR` with flag `0x08` in `desc >> 48`.

The helper allocates counters itself rather than via kpep's allocator: each event's allowed counters (`kpep_event.mask`, by absolute PMC) and counter-independent config word come from kpep, and events go on PMCs 5–7 first. `PERF_KPC_HNDLR` is emitted before the pid filter, so the helper keeps the last PMI per CPU and attributes it to our pid when kperf's interrupt-time `PERF_GEN_EVENT` (with `SAMPLE_FLAG_PEND_USER`) follows on the same CPU and thread. That also yields the event index and how many PMIs coalesced into one stack (the sample weight). The agent uses the precise PC as the leaf when it lies in the same function as the interrupted PC.

Results (`spike2.sh`):

- `L1D_CACHE_MISS_LD_NONSPEC` on PMC5: 24271/24271 stacks had a precise PC; 99.3% were used as the leaf (the rest fell outside the leaf function). On PMC2 (capture disabled): 0.
- `CORE_ACTIVE_CYCLE` on PMC5: 0/90017. **Capture depends on the event, not just the counter**; it works for the load-miss event. `INST_ALL` was only tested alongside other events, so it is unknown.
- In a three-event session (PMC5/6/7) the precise count (24187) matched the cache-miss samples (24194), consistent with the above rather than with the single capture register being clobbered.

Attribution of L1D load misses, by leaf:

| leaf | skid (PMC2) | precise (PMC5) |
|---|---|---|
| `Workload.step` (inlined load) | 31.4% | 35.3% |
| `Workload.main` (setup shuffle) | 13.4% | 27.6% |
| `Workload.chase` | 8.7% | 4.9% |
| `AtomicLong.compareAndSet` + `Random.nextInt`/`next` | 14.6% | ~0.1% |

The shuffle in `main` misses on its `next[j]` loads; with skid, ~15% of misses land in the `Random` code it calls next. Precise PCs put them back on the loads, and move misses from the `chase` loop onto the inlined `step` load.

## Which events capture, per-event stats, JFR

- **Capture support is in kperfdata's database:** `pc_capture_counters_mask` on 19 of 136 events in `/usr/share/kpep/cpu_100000c_2_f76c5b1a.plist` (M5 Pro). That covers branch mispredicts, L1D load/store misses, TLB misses and load sources, all `0xe0` (PMC 5–7), plus `INST_MICROCODED` (`0x80`). There is nothing for cycles or instructions. `kpep_event` doesn't expose the field, so the helper reads the plist (found by `hw.cputype`/`cpusubtype`/`cpufamily`).
- **Allocation is three-pass:** capture-capable events go on their capture PMCs, then other events on PMCs no requested event could capture on, then anything left over.
  - `cycles+cache-misses+instructions+branch-misses` → PMC2 / **PMC5** / PMC3 / **PMC6**.
- **Per-event capture rates** (helper, 15s):

| event | PMC | precise PC | used as leaf (agent) |
|---|---|---|---|
| `cache-misses` | 5 | 100% | 99.2% |
| `branch-misses` | 6 | 98% | ~55–60% |
| `cycles` / `instructions` | 2 / 3 | 0% (not capable) | — |

  For branch mispredicts, ~40% of precise PCs lie outside the interrupted PC's function. A mispredicted call or return transfers control before the PMI, so the latched PC is in the caller or callee; the agent currently drops those.
- **PMIs of different events can coalesce into one stack.** The helper sends one sample per event, sharing the stack, each with its own PMI count and captured PC. Otherwise all the counts go to whichever event fired last; cache-misses was overstated by ~3% before this fix.
- **`profiler.HardwareSample` JFR event:** `startTime`, `sampledThread`, `stackTrace`, `event` (as requested, e.g. `cache-misses`), `value` (period × PMIs) and `precise`. `jdk.ExecutionSample` has no weight field, so the event count was lost as well as the name. The JDK's `jfr print` reads the events, and per-event `value` sums equal PMIs × period exactly.
- **`jfrconv --hw <event>`** selects one event's samples and weights them by event count (`--total` implied); the units are `events`.

## Multiple events and exclusivity

- `-e cycles+cache-misses+instructions` (up to 4, optional `@period` each) runs in one session on separate PMCs; the stack root carries the event name. The per-event profiles match the single-event runs.
- A second `asprof -e cycles` while the helper server was sampling another JVM failed cleanly: `asprof-kperf: PMU counters are reserved by another process (Instruments, xctrace or another profiler?)`, exit status 200.

## Future work

1. ~~Precise PC capture~~, ~~event-aware allocation~~, ~~per-event capture rates~~ (done). Remaining: for branch mispredicts, use a precise PC that is outside the leaf function as an extra leaf frame instead of dropping it.
2. ~~Helper exclusivity check~~ (done).
3. **`benchLabAsync` hook** (scala3 bench-lab). Add `ASYNC_PROFILER_KPERF` support so JMH forks can use a long-running helper (`sudo asprof-kperf -s /tmp/asprof-kperf.sock` plus `-prof "async:libPath=…;event=…;rawCommand=kperf=/tmp/asprof-kperf.sock"`) without fighting sbt's `;` splitting.
4. ~~Multiple events per session~~, ~~JFR event with name and value~~ (done). Remaining: per-stack ratios (e.g. misses per kilo-instruction) in the converter; `--hw` support in the heatmap/pprof/OTLP converters beyond what `JfrConverter` gives them.
5. **Use kernel timestamps,** converted to async-profiler's clock.
6. **Verify the method at resolve time,** e.g. check it is still live at the sample's timestamp, or track compiled-method load and unload events.
7. **Cheaper thread-id mapping.** Look up only the missing thread instead of re-listing all of them.
8. **Kernel stacks** (`KPERF_SAMPLER_KSTACK`): cheap to collect, but need kernel symbolication.
10. **Upstream hygiene:** automated tests gated on root and real hardware; docs in `CpuSamplingEngines.md`; signing and notarizing `asprof-kperf`; Intel Mac testing; a launchd-daemon option to avoid `sudo` per session.
