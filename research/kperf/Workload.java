import java.util.Random;

// Three hot loops with distinct PMU signatures, so different events should
// give visibly different profiles:
//   chase()  - random pointer chasing over 256MB: dominated by cache/TLB misses
//   scan()   - sequential sum over the same array: high IPC, prefetch-friendly
//   mix()    - pure ALU hashing, no memory traffic
public class Workload {
    static final int N = 64 * 1024 * 1024;
    static final int[] next = new int[N];
    static volatile long sink;

    public static void main(String[] args) {
        long seconds = args.length > 0 ? Long.parseLong(args[0]) : 60;
        Random rnd = new Random(42);
        // Sattolo's algorithm: a single random cycle through all N slots.
        for (int i = 0; i < N; i++) next[i] = i;
        for (int i = N - 1; i > 0; i--) {
            int j = rnd.nextInt(i);
            int t = next[i]; next[i] = next[j]; next[j] = t;
        }
        System.out.println("pid " + ProcessHandle.current().pid() + " ready");
        long deadline = System.nanoTime() + seconds * 1_000_000_000L;
        int p = 0;
        while (System.nanoTime() < deadline) {
            p = chase(p, 2_000_000);
            sink += scan();
            sink += mix(p, 20_000_000);
        }
    }

    static int chase(int p, int steps) {
        for (int i = 0; i < steps; i++) p = step(p);
        return p;
    }

    // Small enough to be inlined into chase(): exercises inlined frame resolution
    static int step(int p) {
        return next[p];
    }

    static long scan() {
        long s = 0;
        for (int i = 0; i < N; i++) s += next[i];
        return s;
    }

    static long mix(long x, int iters) {
        for (int i = 0; i < iters; i++) {
            x ^= x << 13; x ^= x >>> 7; x ^= x << 17;
            x *= 0x9E3779B97F4A7C15L;
        }
        return x;
    }
}
