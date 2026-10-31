# kperf PMI sampling prototype (macOS)

For which metric and tool to use when benchmarking the compiler, see [WHICH-TOOL.md](WHICH-TOOL.md).

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

## Choosing a period

The period (`-e cache-misses@100000`, or `-i` for a single event) is the number of events per sample, not a time rate. A busy JVM produces hundreds of millions of L1D misses a second, so the sample count follows the workload, and a period that suits a small loop can bury a real one. Choose it for a target sample count per profile: **period ≈ events per op × ops / target**. 50–100k samples per profile is plenty for flame graphs and leaf attribution. Each sample costs about 1 KB of collapsed stacks and 0.7 KB of JFR on a compiler workload, and precise-PC events (misses and branch mispredicts on PMCs 5–7, below) also carry a captured PC for the agent to resolve.

A scala3 compiler benchmark, for example, has ~470M L1D load misses per op. At the old `cache-misses` default of 10,000, 13 warmup and 16 measured ops gave ~970k samples per fork, a 650 MB JFR, and gigabytes of collapsed stacks per event. `@100000` brings that to ~97k.

Defaults, measured with javac compiling this repo's Java sources 300 times in one JVM (~28s, about one JMH fork; scratch driver, not checked in):

| event | kpep name | old period → samples | new period → samples |
|---|---|---|---|
| `cycles`, `cpu` | `CORE_ACTIVE_CYCLE` | 1M → 133k | 2M → 71k |
| `instructions` | `INST_ALL` | 1M → 340k | 5M → 66k |
| `cache-misses` | `L1D_CACHE_MISS_LD_NONSPEC` | 10k → 356k | 100k → 37k |
| `branch-misses` | `BRANCH_MISPRED_NONSPEC` | 10k → 149k | 20k → 75k |
| raw miss event, e.g. `PL2_CACHE_MISS_LD` | | 1M → 1.3k | 100k → 13k |

- `cache-misses` is set by the heavier scalac data point rather than javac's: 100k gives 37k for javac and ~97k for scalac, where 50k would give 71k and ~194k.
- **A kpep name without `@period`** gets its alias's default if it has one (`INST_ALL` → 5M). Otherwise the default goes by kind: 100k if the name contains `MISS` or `MISPRED`, 2M for anything else. Rare events such as TLB misses may still want a smaller period.
- **Above 500k samples of one event** the agent warns at the end of the session and suggests a period for ~100k, e.g. `kperf: cache-misses: 777256 samples make a large profile; try cache-misses@40000`.

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

## Counting mode, sampling overhead and IPC vs xctrace

**Question.** The scalac runs measured lower IPC than xctrace. Is our own sampling the cause? Two theories: the kernel's work for each sample (PMI handler, AST-time stack walk) is counted and attributed to the sampled thread, or the interrupts disturb caches and branch predictors for the user code.

**Counting mode.**

- Counters count user mode only by default (EL0 A64 in each config word). `KPERF_KERNEL=1` in the agent, or `-k` on the helper, adds EL1 (`0x40000`).
- kpep's config words carry no mode bits. On this machine that means **user mode only**, not all modes: with `-k` clearing the bits, the read-back was `0x2` and every PMI carried flag `0x4`. So every run so far, including the earlier "user+kernel" ones, counted user mode only. That's why kernel vs user-only made no difference.
- XNU reports each PMI's counting mode in the `PERF_KPC_HNDLR` desc flags: `0x2` kernel counting, `0x4` user counting, `0x8` captured PC. The helper prints their union per event. User-only runs show `0x4` (`0xc` with PC capture), as expected. Setting EL1 explicitly doesn't help either: the helper set `0x6`, but the read-back was `0x2` and the PMI flags `0x4`. XNU strips kernel counting: `set_modes` in `osfmk/arm64/kpc.c` allows EL1 only if `kpc_allows_counting_system`, which is `PE_i_can_has_debugger()`, i.e. only with kernel debugging enabled. **Kernel counting is unavailable here**, and `-k` / `KPERF_KERNEL=1` are no-ops. xctrace goes through the same kernel, so it very likely counts user mode only too.
- `kpc_get_config` read-back is unreliable: PMC 5/6 read back `0` whatever was set. The PMI flags are the trustworthy signal.
- The "user+kernel" runs below were therefore also user-only.

**Synthetic workload** (`overhead.sh`, `Workload`, 15 s, DRAM-bound, IPC 0.25):

| run | PMIs/s | rounds/s | IPC |
|---|---|---|---|
| no profiler | — | 3.40 | — |
| 4 events, default periods, user / user+kernel | ~7.6k | 3.38 / 3.30 | 0.25 / 0.25 |
| 4 events, `cycles@100000…@1000`, user / user+kernel | ~69k | 2.97 / 3.05 | 0.31 / 0.31 |

- At ~70k PMIs/s throughput drops 10–13%, about 1.5 µs per PMI including its stack walk.
- IPC *rises*, because counted cycles drop (72G → 59G) while instructions don't: time spent handling PMIs is not counted. At high rates, user-mode IPC is overstated rather than depressed. This workload is DRAM-bound, so it can't show cache or predictor pollution.

**scalac** (dotc `CompileBenchmark`, `dottyUtil`, JDK 25, JMH 1 fork, 4×3 s measured, ~2.5–3 cores of background load; IPC on the benchmark thread):

| run | PMIs/s (measured window) | IPC |
|---|---|---|
| xctrace ipc template (JMH's), ×3 | — | 2.73 (2.64–2.82) |
| `cycles+instructions`, 1M, ×3 interleaved | ~20k | 2.56 (2.52–2.59) |
| `cycles@10000000+instructions@10000000`, ×3 interleaved | ~2k | 2.60 (2.56–2.70) |
| + `cache-misses`/`branch-misses` @100k / @10k, ×1 each | up to ~40k | 2.46 / 2.43 |

- **The sampling period doesn't measurably change IPC.** 10x fewer PMIs moved IPC by ~2%, within run-to-run spread. Adding 230k miss PMIs didn't lower it either. The "kernel vs user-only" runs (both actually user-only) made no visible difference (2.25 vs 2.44 and 3.98 vs 3.89, single runs on two corpora).
- **The ~5% gap to xctrace was noise, not bias.** Both tools sample `CORE_ACTIVE_CYCLE` and `INST_ALL`, and both are very likely user-only. A later run on a quieter machine (1.8 cores of background load) gave a bench-thread IPC of 2.71, against xctrace's 2.73. kperf counters run per CPU, so a PMI credited to the benchmark thread includes events from whatever else ran on that core since the last overflow (GC, other processes). That pulls the benchmark thread's IPC toward the machine average. xctrace uses exact per-thread counter deltas.
- **Per-thread check (`-t`).** Exact per-thread counts (kpc thread counting plus the `PMC_THREAD` sampler) vs PMI attribution, in the measured window:

| | exact | PMI-attributed |
|---|---|---|
| bench thread IPC | 2.686 | 2.707 |
| process IPC | 2.630 | 2.641 |
| process cycles / instructions | 7.05e10 / 1.85e11 | 96.1% / 96.5% of exact |

  - Crediting each PMI's whole period to the thread running at overflow is unbiased: per-thread IPC agrees within 1%.
  - In that run the helper missed some events: 3.5–4% in the measured window, and 9–12% during JIT-heavy warmup. Cycles and instructions lost about equally, so ratios held.
  - A rerun with loss diagnostics lost nothing: PMI-attributed counts were 99.9–100.4% of exact. That run had no kdebug lost-events markers and no unmatched PMIs on our threads. Weighting each PMI by its actual counter delta (the cumulative count in `PERF_KPC_HNDLR`) gave exactly 100% of PMIs × period, so overflows are never late or coalesced. The earlier loss was most likely kdebug buffer wrap, which drops PMI records while the exact counts (first/last snapshots) survive. The earlier build couldn't report it, and the helper now prints lost-events markers every session.
  - The fixed counters (0/1) count 4.9% more cycles and 2.7% more instructions than the user-only configurable counters (IPC 2.575 vs 2.630). They count all modes: `monotonic` owns them, and kpc sets their PMCR1 bits for EL0 and EL1 (`0x30303`). They can't be sampled, though: kpc's reload path programs periods for the configurable counters only. So kernel time is visible only in aggregate, as fixed minus configurable per thread.
- **Earlier "low IPC" figures were mostly noise and conditions:** single runs, JDK 21 vs 25, shorter warmup, background load.
- **JMH's async-profiler integration starts the profiler twice:** at the first warmup iteration, and again (`stop`, `start`) at the first measured iteration. Each fork therefore shows two helper sessions, and the second is the measured window.

## Future work

1. ~~Precise PC capture~~, ~~event-aware allocation~~, ~~per-event capture rates~~ (done). Remaining: for branch mispredicts, use a precise PC that is outside the leaf function as an extra leaf frame instead of dropping it.
2. ~~Helper exclusivity check~~ (done).
3. **`benchLabAsync` hook** (scala3 bench-lab). Add `ASYNC_PROFILER_KPERF` support so JMH forks can use a long-running helper (`sudo asprof-kperf -s /tmp/asprof-kperf.sock` plus `-prof "async:libPath=…;event=…;rawCommand=kperf=/tmp/asprof-kperf.sock"`) without fighting sbt's `;` splitting.
4. ~~Multiple events per session~~, ~~JFR event with name and value~~ (done). Remaining: per-stack ratios (e.g. misses per kilo-instruction) in the converter; `--hw` support in the heatmap/pprof/OTLP converters beyond what `JfrConverter` gives them.
5. **Use kernel timestamps,** converted to async-profiler's clock.
6. **Verify the method at resolve time,** e.g. check it is still live at the sample's timestamp, or track compiled-method load and unload events.
7. **Cheaper thread-id mapping.** Look up only the missing thread instead of re-listing all of them.
8. **Kernel stacks** (`KPERF_SAMPLER_KSTACK`): cheap to collect, but need kernel symbolication.
9. ~~Close the IPC gap to xctrace~~ (noise; attribution verified with `-t`). Lost PMIs are now reported (kdebug wrap markers, unmatched PMIs). Remaining: if wrap recurs, enlarge the kdebug buffer or read more often, and forward a lost-events count to the agent. Exact per-thread counts could also weight ratios directly instead of PMI counts.
10. **Upstream hygiene:** automated tests gated on root and real hardware; docs in `CpuSamplingEngines.md`; signing and notarizing `asprof-kperf`; Intel Mac testing; a launchd-daemon option to avoid `sudo` per session.
