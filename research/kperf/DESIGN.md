# Hardware-counter profiling on macOS via kperf

## Problem

On Linux, `asprof -e cache-misses` gives Java flame graphs weighted by a PMU event. On macOS there is no equivalent: async-profiler has no hardware events there (async-profiler#182), because the PMU is only reachable through private kernel interfaces. The tools that do reach it are poor fits for JVM work. Instruments/xctrace cannot name JIT frames, and JMH's `XCTraceAsmProfiler` attributes only leaf PCs to disassembly, with no call stacks.

The prototype in this directory (`kperf_sample.c`, see README) shows the kernel side is usable directly. A root process can program a counter to interrupt every N events and have XNU capture the frame-pointer user stack of the interrupted thread, filtered to one pid. On an M5 Pro this produced correct, full native-to-Java stacks for cycles, instructions and L1D misses.

What the prototype lacks is good Java symbolication: no inlined frames, interpreted frames unknown, and JIT names taken from periodic `jcmd` snapshots that race with code-cache churn. async-profiler already solves exactly this problem in-process. The goal is to move sample resolution into the agent and make this a normal async-profiler event, with JFR, flame graph and collapsed output.

## Proposal

Split the work between a privileged **kperf helper** and an unprivileged **agent engine**, connected by a local socket.

- The **helper** runs as root. It programs the PMU and kperf, reads the kdebug trace buffer, and streams compact raw samples (timestamp, thread, counter, return-address array) for the target pid only.
- The **agent engine** runs inside the JVM. It consumes the stream on a dedicated thread, resolves each address array against the live JVM, and records the result as a `PERF_SAMPLE` via `Profiler::recordExternalSample`.

From the user's point of view:

```bash
sudo asprof -e L1D_CACHE_MISS_LD_NONSPEC -i 20000 -d 30 -f out.html <pid>
```

`asprof` already runs as root here and already knows how to attach to a JVM owned by another user. It forks the helper (keeping root) and then loads the agent as the target's user, passing the helper's socket path in the agent arguments.

## Key decisions

**Resolve in the agent, not the helper.** Only the agent can see the code cache and VMStructs at the moment a sample arrives. Resolving there gives inlined frames through the existing `NMethod::findScopeOffset` and `ScopeDesc` decoding, real `jmethodID`s, C1/C2 frame types, and stub names, all for free. The helper stays a dumb, auditable pipe, and nothing JVM-specific runs as root. The cost is that resolution is not simultaneous with the sample. See *Staleness* below.

**Kernel-captured stacks, not in-process stack walking.** On Linux, a perf_events overflow is delivered as a signal and async-profiler walks the stack in the handler. kperf has no signal action: the PMI is handled in the kernel and the user stack is captured on the AST before returning to user space. So we take the kernel's frame-pointer walk as the stack. This requires `-XX:+PreserveFramePointer`, which the engine should check and warn about. The helper has no other way to see Java frames.

**Mirror an existing pattern: `J9StackTraces`.** That component already does almost exactly this. A background thread receives raw native PC arrays from outside the sampled thread, turns them into frames, and calls `recordExternalSample`. The new engine has the same shape, plus a code-cache-aware frame resolver in place of `convertNativeTrace`.

**Mirror an existing pattern: `fdtransfer`.** On Linux, `asprof --fdtransfer` already runs a privileged helper that serves an unprivileged agent over a unix socket. The kperf helper should reuse its lifecycle: spawned by `asprof`, its path passed via an agent option, one target per helper. We can't pass fds here, but the process model, options and docs story are the same.

**Generic helper protocol, agent owns configuration.** The agent sends a start request carrying the event names and periods, and the helper validates them against its kpep database. That keeps event syntax and validation in one place (`-e`/`-i` parsing in the agent) and lets the helper reject anything beyond PMI sampling for the peer's own pid.

**Security boundary is the peer pid.** The helper samples only the pid of the connected peer, obtained with `getsockopt(LOCAL_PEERPID)`, which must match the pid `asprof` attached to. It ignores any pid in the request, and the socket lives in a directory only the target uid can enter. A compromised or curious JVM can therefore only profile itself.

**Thread identity.** kdebug reports 64-bit `thread_id`s, while async-profiler on macOS identifies threads by mach port name (`OS::threadId`). The engine keeps a `thread_id → port` map built from `task_threads` + `thread_info(THREAD_IDENTIFIER_INFO)`, refreshed on a miss, so JFR samples join up with thread names and other events.

## Frame resolution

The kernel gives `pc, ret1, ret2, ...` (leaf first), plus the interrupted LR as a fixup value. For each address, from the leaf outwards:

- **Compiled nmethod:** unfold inlined scopes via `findScopeOffset`/`ScopeDesc`. Return addresses map exactly to a call-site `PcDesc`. The leaf PC maps to the nearest `PcDesc`, the same approximation `cstack=vm` makes today; `-XX:+DebugNonSafepoints` improves it.
- **Leaf in an nmethod prologue/epilogue** (`!isFrameCompleteAt(pc)`): the frame record hasn't been pushed, so the kernel's walk skips the caller. Insert the fixup LR as the caller.
- **Interpreter:** we only have the PC, not the frame pointer, so the `Method*` is unrecoverable. Emit a single `[interpreted]` frame. Hot code is compiled, so this matters mostly for startup.
- **Stubs and adapters:** named via `findRuntimeStub` / the blob name, as `walkVM` does.
- **Native:** `findNativeMethod`, with the usual marks applied, e.g. truncating at `MARK_INTERPRETER` is wrong here and must be skipped.

This is a stripped-down `walkVM` that consumes a list of PCs instead of unwinding from a ucontext. It should share the per-frame classification code rather than duplicate it.

## Staleness

The sample is resolved milliseconds after it was taken, by which time code may have been unloaded or relocated. Mitigations, in order of cost:

- Keep latency low: the helper reads kdebug continuously and forwards immediately, and the agent thread blocks on the socket.
- nmethods are freed only after a code-cache sweep or a GC unloading cycle, which is much longer than our latency. A cheap sanity check (the PC is inside `nm`, and `nm` is still `in_use`) drops the rare mismatch as an error frame instead of misattributing it.
- If this proves insufficient, record `CompiledMethodLoad`/`Unload` events and resolve against the code-cache state at the sample's timestamp.

## Supporting changes

- **asprof:** a macOS kperf helper mode alongside `FdTransferServer`. It handles the kpep event database, kpc/kperf programming, the kdebug reader, the socket server and clean teardown on exit or signal (kperf and kdebug are global, single-owner state).
- **Agent:** a `KperfEvents` engine selected for hardware event names on macOS. It holds the socket client, the thread-id map and the PC-array resolver.
- **Event names:** accept kpep names directly (`L1D_CACHE_MISS_LD_NONSPEC`), plus aliases for the common Linux names (`cycles`, `instructions`, `cache-misses`, `branch-misses`), so existing command lines keep working.
- **Docs:** a macOS section in `CpuSamplingEngines.md` covering root, `PreserveFramePointer` and the conflict with a concurrent Instruments/ktrace session.

## Prior art

- **async-profiler `J9StackTraces`:** out-of-thread conversion of raw PC arrays into recorded samples.
- **async-profiler `fdtransfer`:** a privileged helper serving an unprivileged agent.
- **Linux `perf` + `perf-map-agent` / `-XX:+DumpPerfMapAtExit`:** kernel-captured FP stacks resolved against a JIT symbol map after the fact. This is what the prototype does and the design improves on, because resolution happens against the live VM and gets inlining.
- **Instruments CPU Counters / `xctrace`:** the same kperf PMI machinery, but no JIT symbolication.
- **JMH `XCTraceAsmProfiler`:** xctrace PMI sampling joined with `PrintAssembly` output; leaf PCs only.
- **ibireme `kpc_demo`, `tmcgilchrist/mperf`:** reference usage of the private kpc/kpep frameworks, counting only.

## Out of scope / future work

- **Precise PC capture** (`kpc.pc_capture_supported`; PMCs 5–7 on Apple silicon): more accurate leaf attribution for memory events. The helper can pass the captured PC through once we learn to steer events onto those counters.
- **Multiple events in one session:** one kperf action per counter; the PMI record already carries the counter index.
- **Kernel stacks** (`KPERF_SAMPLER_KSTACK`): cheap to add, but kernel addresses are unslid and need kernelcache symbolication.
- **Intel Macs:** the kpc/kperf API is the same, but untested.
- **Running without root:** not possible without Apple-only entitlements. A long-running root helper (launchd daemon) could remove the per-session `sudo`, but that is a deployment question, not a design one.

## Open questions

- Should the helper live in `asprof` (simplest distribution, mirrors fdtransfer) or be a separate small binary that is easier to audit and sign?
- How do we make the leaf-frame fixup robust for frameless native leaves without DWARF? Instruments uses a heuristic; is "insert LR if it lies outside the leaf function and isn't already `frames[1]`" good enough?
- Is `PERF_SAMPLE` the right JFR event type, or should these get a distinct event carrying the kpep event name?
