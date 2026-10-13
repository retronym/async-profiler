# Which tool to choose (Scala compiler benchmarking on macOS)

## The problem

Wall-clock benchmarks of the Scala compiler are noisier than the changes we want to measure. On a dev Mac, `ms/op` routinely has ±20–35% error bars over a few iterations. Background load (other sessions, endpoint agents such as JumpCloud and ESET) adds CPU contention and I/O interference that a single change can't be separated from.

So measure in two steps:

1. A **low-variance metric** decides whether a change helped.
2. A **profile** explains where and why.

Wall clock is the final confirmation, on a quiet machine, not the day-to-day signal.

## Metrics, from least to most noisy

| metric | tool | what it's insensitive to | blind spots |
|---|---|---|---|
| `gc.alloc.norm` (bytes allocated per op) | JMH `-prof gc` | CPU frequency, scheduling, cache state, other load | Compute that doesn't allocate |
| Retired user-mode instructions per op, on compiler threads | xctrace via JMH `-prof xctracenorm`, or bench-lab's `XctraceProfiler` | Frequency, stalls, other load, kernel and I/O interference | Memory stalls: fewer instructions can still be slower |
| User-mode cycles per op | Same | Frequency (it counts cycles, not time), kernel and I/O | Cache and memory-bandwidth contention from other load |
| IPC, misses per 1k instructions | Same, or kperf events | — | Ratios that explain a cycles change; not a target in themselves |
| `ms/op` | JMH | — | Everything adds noise: frequency, load, I/O, GC timing |

On this kind of machine, user-mode-only counting is an advantage rather than a gap. It drops the syscall, page-fault and I/O-interference time that differs run to run and that a compiler change rarely affects.

## Profiles: where the cost is

| question | tool |
|---|---|
| Which code executes the most instructions / cycles (user mode)? | async-profiler `-e instructions` / `-e cycles` (kperf). Full Java stacks, including inlined frames. |
| Which code misses cache or mispredicts branches? | async-profiler `-e cache-misses` / `-e branch-misses`. Precise PC: misses are attributed to the load or branch itself, without skid. |
| Several at once, with ratios per stack | `-e cycles+instructions+cache-misses`, JFR output, `jfrconv --hw <event>` |
| Where does CPU time go, including syscalls and page faults? | async-profiler `-e cpu` / `itimer` |
| Where do threads wait (locks, I/O)? | async-profiler `-e wall` |
| Leaf-PC annotation of hot JIT code only | JMH `-prof xctraceasm` |

kperf events are user mode only. A kernel-heavy method looks cheap in a `cycles` profile. For scalac (`dottyUtil`), the kernel is roughly 5% of on-CPU cycles, measured as the fixed counters' cycles minus the user-only counters' cycles. Use `cpu` when syscalls or page faults are the suspect.

## Why not kernel-inclusive counter sampling

On Apple silicon with a release kernel:

- **The configurable counters can't count kernel mode** unless kernel debugging is enabled: XNU allows EL1 counting only when `PE_i_can_has_debugger()` is true.
- **The fixed counters do count all modes** (`monotonic` owns them), **but can't be sampled**: kpc programs periods for configurable counters only.

So kernel time is visible only in aggregate (fixed minus configurable, per thread), not as sampled stacks.

## Trust

- **kperf IPC agrees with xctrace** (2.56–2.78 vs 2.64–2.82 on `dottyUtil`), once machine load and run-to-run noise are accounted for.
- **PMI attribution is unbiased:** per-thread exact counts and PMI-attributed counts agree within 1% (`asprof-kperf -t`).
- **Sampling overhead is negligible at the default periods:** a 10x longer period changes IPC by under the noise.

See `README.md` for the experiments behind each point.
