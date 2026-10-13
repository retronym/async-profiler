/*
 * kperf_sample: PMU-overflow (PMI) sampling of a running process on macOS,
 * without Instruments, using the private kperf/kperfdata frameworks and the
 * raw kdebug trace buffer. Prototype for feeding hardware-counter samples to
 * async-profiler.
 *
 * Mechanism (see XNU osfmk/kperf, osfmk/kern/kpc_common.c, tests/cpu_counters):
 *   - kperfdata (kpep_*) translates an event name into a PMU config word.
 *   - kpc_set_config/kpc_set_period program a configurable counter to overflow
 *     every N events; kpc_set_actionid binds that overflow (PMI) to kperf
 *     action 1.
 *   - Action 1 samples the user callstack (KPERF_SAMPLER_USTACK), filtered to
 *     the target pid. The stack is walked by the kernel (frame pointers) in an
 *     AST on return to user space, and emitted into kdebug as
 *     PERF_CS_UHDR + N x PERF_CS_UDATA (4 frames each), tagged with the tid.
 *   - We read kdebug via sysctl(KERN_KDEBUG, KERN_KDREADTR).
 *
 * Symbolication (prototype quality):
 *   - JIT frames: periodic `jcmd <pid> Compiler.codelist` (run as the target's
 *     uid). No inlining info: a frame is attributed to the outermost nmethod.
 *   - Native frames: `atos -p <pid>` at the end, while the target is alive.
 *
 * Output: collapsed stacks (thread;root;...;leaf count), consumable by
 * async-profiler's jfrconv / FlameGraph.
 *
 * Requires root. Run the target JVM with -XX:+PreserveFramePointer.
 */

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <libproc.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* ---------------------------------------------------------------- kpc/kperf */

#define KPC_CLASS_FIXED_MASK        (1u << 0)
#define KPC_CLASS_CONFIGURABLE_MASK (1u << 1)
#define KPC_MAX_COUNTERS            32

#define KPERF_SAMPLER_TH_INFO (1u << 0)
#define KPERF_SAMPLER_USTACK  (1u << 3)

typedef uint64_t kpc_config_t;

static int (*kpc_force_all_ctrs_set)(int);
static int (*kpc_force_all_ctrs_get)(int *);
static int (*kpc_set_counting)(uint32_t);
static int (*kpc_set_thread_counting)(uint32_t);
static int (*kpc_set_config)(uint32_t, kpc_config_t *);
static int (*kpc_set_period)(uint32_t, uint64_t *);
static int (*kpc_set_actionid)(uint32_t, uint32_t *);
static uint32_t (*kpc_get_counter_count)(uint32_t);
static uint32_t (*kpc_get_config_count)(uint32_t);

static int (*kperf_action_count_set)(uint32_t);
static int (*kperf_action_count_get)(uint32_t *);
static int (*kperf_action_samplers_set)(uint32_t, uint32_t);
static int (*kperf_action_filter_set_by_pid)(uint32_t, int32_t);
static int (*kperf_sample_set)(uint32_t);
static int (*kperf_reset)(void);

/* kperfdata; struct layouts are opaque here, we only pass pointers around. */
typedef struct kpep_db kpep_db;
typedef struct kpep_config kpep_config;
typedef struct kpep_event kpep_event;

static int (*kpep_db_create)(const char *, kpep_db **);
static void (*kpep_db_free)(kpep_db *);
static int (*kpep_db_name)(kpep_db *, const char **);
static int (*kpep_db_events_count)(kpep_db *, size_t *);
static int (*kpep_db_events)(kpep_db *, kpep_event **, size_t);
static int (*kpep_db_event)(kpep_db *, const char *, kpep_event **);
static int (*kpep_event_name)(kpep_event *, const char **);
static int (*kpep_event_description)(kpep_event *, const char **);
static int (*kpep_config_create)(kpep_db *, kpep_config **);
static void (*kpep_config_free)(kpep_config *);
static int (*kpep_config_force_counters)(kpep_config *);
static int (*kpep_config_add_event_trigger)(kpep_config *, kpep_event **, uint32_t, uint64_t, uint32_t *);
static int (*kpep_config_kpc_classes)(kpep_config *, uint32_t *);
static int (*kpep_config_kpc)(kpep_config *, kpc_config_t *, size_t);
static int (*kpep_config_kpc_periods)(kpep_config *, uint64_t *, size_t);
static int (*kpep_config_kpc_map)(kpep_config *, size_t *, size_t);

#define LOAD(lib, sym)                                                        \
    do {                                                                      \
        *(void **)&sym = dlsym(lib, #sym);                                    \
        if (!sym) { fprintf(stderr, "missing symbol %s\n", #sym); return false; } \
    } while (0)

static bool load_frameworks(void) {
    void *kperf = dlopen("/System/Library/PrivateFrameworks/kperf.framework/kperf", RTLD_LAZY);
    void *kpep = dlopen("/System/Library/PrivateFrameworks/kperfdata.framework/kperfdata", RTLD_LAZY);
    if (!kperf || !kpep) {
        fprintf(stderr, "dlopen: %s\n", dlerror());
        return false;
    }
    LOAD(kperf, kpc_force_all_ctrs_set);
    LOAD(kperf, kpc_force_all_ctrs_get);
    LOAD(kperf, kpc_set_counting);
    LOAD(kperf, kpc_set_thread_counting);
    LOAD(kperf, kpc_set_config);
    LOAD(kperf, kpc_set_period);
    LOAD(kperf, kpc_set_actionid);
    LOAD(kperf, kpc_get_counter_count);
    LOAD(kperf, kpc_get_config_count);
    LOAD(kperf, kperf_action_count_set);
    LOAD(kperf, kperf_action_count_get);
    LOAD(kperf, kperf_action_samplers_set);
    LOAD(kperf, kperf_action_filter_set_by_pid);
    LOAD(kperf, kperf_sample_set);
    LOAD(kperf, kperf_reset);
    LOAD(kpep, kpep_db_create);
    LOAD(kpep, kpep_db_free);
    LOAD(kpep, kpep_db_name);
    LOAD(kpep, kpep_db_events_count);
    LOAD(kpep, kpep_db_events);
    LOAD(kpep, kpep_db_event);
    LOAD(kpep, kpep_event_name);
    LOAD(kpep, kpep_event_description);
    LOAD(kpep, kpep_config_create);
    LOAD(kpep, kpep_config_free);
    LOAD(kpep, kpep_config_force_counters);
    LOAD(kpep, kpep_config_add_event_trigger);
    LOAD(kpep, kpep_config_kpc_classes);
    LOAD(kpep, kpep_config_kpc);
    LOAD(kpep, kpep_config_kpc_periods);
    LOAD(kpep, kpep_config_kpc_map);
    return true;
}

/* ------------------------------------------------------------------- kdebug */

typedef struct {
    uint64_t timestamp;
    uint64_t arg1, arg2, arg3, arg4;
    uint64_t arg5; /* thread id */
    uint32_t debugid;
    uint32_t cpuid;
    uint64_t unused;
} kd_buf;

typedef struct {
    unsigned int type;
    unsigned int value1, value2, value3, value4;
} kd_regtype;

#define KDBG_CLASSTYPE 0x10000
#define DBG_PERF       37
#define KDBG_EVENTID(c, s, code) (((uint32_t)(c) << 24) | ((uint32_t)(s) << 16) | ((uint32_t)(code) << 2))
#define DBG_FUNC_START 1u
#define DBG_FUNC_END   2u
#define DEBUGID_MASK   (~3u)

#define PERF_CALLSTACK 2
#define PERF_KPC       6
#define PERF_CS_UDATA  KDBG_EVENTID(DBG_PERF, PERF_CALLSTACK, 4)
#define PERF_CS_UHDR   KDBG_EVENTID(DBG_PERF, PERF_CALLSTACK, 6)
#define PERF_CS_ERROR  KDBG_EVENTID(DBG_PERF, PERF_CALLSTACK, 7)
#define PERF_KPC_HNDLR KDBG_EVENTID(DBG_PERF, PERF_KPC, 0)

#define CALLSTACK_VALID     (1u << 0)
#define CALLSTACK_TRUNCATED (1u << 4)
#define KPC_CAPTURED_PC     0x08 /* flags in desc >> 48 */

static int kd_ctl(int op, int arg, void *buf, size_t *len) {
    int mib[4] = {CTL_KERN, KERN_KDEBUG, op, arg};
    return sysctl(mib, arg >= 0 ? 4 : 3, buf, len, NULL, 0);
}

static int kdebug_setup(int nbufs) {
    size_t z = 0;
    kd_ctl(KERN_KDREMOVE, -1, NULL, &z);
    if (kd_ctl(KERN_KDSETBUF, nbufs, NULL, &z)) return -1;
    if (kd_ctl(KERN_KDSETUP, -1, NULL, &z)) return -1;
    kd_regtype kdr = {.type = KDBG_CLASSTYPE, .value1 = DBG_PERF, .value2 = DBG_PERF + 1};
    size_t len = sizeof(kdr);
    if (kd_ctl(KERN_KDSETREG, -1, &kdr, &len)) return -1;
    return kd_ctl(KERN_KDENABLE, 1, NULL, &z);
}

static void kdebug_teardown(void) {
    size_t z = 0;
    kd_ctl(KERN_KDENABLE, 0, NULL, &z);
    kd_ctl(KERN_KDREMOVE, -1, NULL, &z);
}

/* ------------------------------------------------------------------ samples */

#define MAX_FRAMES 256

typedef struct {
    uint64_t tid;
    uint32_t nframes;
    uint64_t *frames; /* leaf first, as the kernel emits them */
    uint64_t count;
} stack_t_;

typedef struct {
    uint64_t tid;
    uint32_t expect;
    uint32_t have;
    uint32_t flags;
    uint64_t frames[MAX_FRAMES + 4];
} pending_t;

static stack_t_ *stacks;
static size_t nstacks, capstacks;
static pending_t *pending;
static size_t npending, cappending;

static struct {
    uint64_t pmis, pmis_pc_captured, stacks, truncated, errors, dropped_partial, bad_hdr;
} stats;

static pending_t *pending_for(uint64_t tid) {
    for (size_t i = 0; i < npending; i++)
        if (pending[i].tid == tid) return &pending[i];
    if (npending == cappending) {
        cappending = cappending ? cappending * 2 : 64;
        pending = realloc(pending, cappending * sizeof(*pending));
    }
    pending_t *p = &pending[npending++];
    memset(p, 0, sizeof(*p));
    p->tid = tid;
    return p;
}

/* Linear-probe hash of (tid, frames) -> count. */
static size_t *stack_index;
static size_t stack_index_cap;

static uint64_t hash_stack(uint64_t tid, const uint64_t *f, uint32_t n) {
    uint64_t h = 1469598103934665603ull ^ tid;
    for (uint32_t i = 0; i < n; i++) h = (h ^ f[i]) * 1099511628211ull;
    return h ^ n;
}

static void add_stack(uint64_t tid, const uint64_t *f, uint32_t n) {
    if ((nstacks + 1) * 2 >= stack_index_cap) {
        size_t ncap = stack_index_cap ? stack_index_cap * 2 : 4096;
        size_t *nidx = malloc(ncap * sizeof(size_t));
        memset(nidx, 0xff, ncap * sizeof(size_t));
        for (size_t i = 0; i < nstacks; i++) {
            size_t j = hash_stack(stacks[i].tid, stacks[i].frames, stacks[i].nframes) & (ncap - 1);
            while (nidx[j] != SIZE_MAX) j = (j + 1) & (ncap - 1);
            nidx[j] = i;
        }
        free(stack_index);
        stack_index = nidx;
        stack_index_cap = ncap;
    }
    size_t j = hash_stack(tid, f, n) & (stack_index_cap - 1);
    while (stack_index[j] != SIZE_MAX) {
        stack_t_ *s = &stacks[stack_index[j]];
        if (s->tid == tid && s->nframes == n && !memcmp(s->frames, f, n * sizeof(*f))) {
            s->count++;
            return;
        }
        j = (j + 1) & (stack_index_cap - 1);
    }
    if (nstacks == capstacks) {
        capstacks = capstacks ? capstacks * 2 : 1024;
        stacks = realloc(stacks, capstacks * sizeof(*stacks));
    }
    stack_t_ *s = &stacks[nstacks];
    s->tid = tid;
    s->nframes = n;
    s->frames = malloc(n * sizeof(*f));
    memcpy(s->frames, f, n * sizeof(*f));
    s->count = 1;
    stack_index[j] = nstacks++;
}

static void finish_pending(pending_t *p) {
    /* The last of the non-async frames is the "fixup" value (LR on arm64),
       appended by callstack_fixup_user(); drop it for now. */
    uint32_t n = p->expect > 0 ? p->expect - 1 : 0;
    if (n > MAX_FRAMES) n = MAX_FRAMES;
    if (p->flags & CALLSTACK_TRUNCATED) stats.truncated++;
    if (n > 0) {
        add_stack(p->tid, p->frames, n);
        stats.stacks++;
    }
    p->expect = 0;
    p->have = 0;
}

static void handle_event(const kd_buf *e) {
    uint32_t id = e->debugid & DEBUGID_MASK;
    uint64_t tid = e->arg5;
    if (id == PERF_KPC_HNDLR && (e->debugid & DBG_FUNC_START)) {
        stats.pmis++;
        if (((e->arg1 >> 48) & KPC_CAPTURED_PC)) stats.pmis_pc_captured++;
    } else if (id == PERF_CS_UHDR) {
        pending_t *p = pending_for(tid);
        if (p->expect && p->have < p->expect) stats.dropped_partial++;
        p->flags = (uint32_t)e->arg1;
        p->expect = (uint32_t)e->arg2;             /* non-async frames incl. fixup */
        uint32_t async_n = (uint32_t)e->arg4;
        p->have = 0;
        if (!(p->flags & CALLSTACK_VALID) || p->expect == 0) {
            p->expect = 0;
            stats.bad_hdr++;
            return;
        }
        /* We ignore async (Swift) frames; but they are still emitted as data. */
        (void)async_n;
    } else if (id == PERF_CS_UDATA) {
        pending_t *p = pending_for(tid);
        if (!p->expect) return;
        uint64_t a[4] = {e->arg1, e->arg2, e->arg3, e->arg4};
        for (int i = 0; i < 4 && p->have < p->expect; i++) {
            if (p->have < MAX_FRAMES + 4) p->frames[p->have] = a[i];
            p->have++;
        }
        if (p->have >= p->expect) finish_pending(p);
    } else if (id == PERF_CS_ERROR) {
        stats.errors++;
    }
}

/* ------------------------------------------------------------ symbolication */

typedef struct {
    uint64_t begin, end;
    uint32_t seq;
    char *name;
} jit_entry;

static jit_entry *jit;
static size_t njit, capjit;
static uint32_t jit_seq;

static char *jit_name(const char *sig, int level) {
    /* "pkg.Cls.method(Lfoo;)V" -> "pkg/Cls.method_[j]" (async-profiler style) */
    char buf[1024];
    size_t n = strcspn(sig, "(");
    if (n >= sizeof(buf) - 8) n = sizeof(buf) - 8;
    memcpy(buf, sig, n);
    buf[n] = 0;
    char *lastdot = strrchr(buf, '.');
    for (char *c = buf; c < lastdot; c++)
        if (*c == '.') *c = '/';
    strcat(buf, level >= 4 || level == 0 ? "_[j]" : "_[1]");
    return strdup(buf);
}

static int jit_cmp(const void *a, const void *b) {
    const jit_entry *x = a, *y = b;
    if (x->begin != y->begin) return x->begin < y->begin ? -1 : 1;
    return x->seq < y->seq ? 1 : x->seq > y->seq ? -1 : 0; /* newest first */
}

static char jcmd_path[PROC_PIDPATHINFO_MAXSIZE];
static uid_t target_uid;
static gid_t target_gid;

static void snapshot_codelist(pid_t pid) {
    if (!jcmd_path[0]) return;
    int fds[2];
    if (pipe(fds)) return;
    pid_t child = fork();
    if (child == 0) {
        dup2(fds[1], 1);
        int devnull = open("/dev/null", 1);
        if (devnull >= 0) dup2(devnull, 2);
        close(fds[0]);
        /* HotSpot attach requires the same euid as the target. */
        if (setgid(target_gid) || setuid(target_uid)) _exit(126);
        char p[32];
        snprintf(p, sizeof(p), "%d", pid);
        execl(jcmd_path, "jcmd", p, "Compiler.codelist", (char *)NULL);
        _exit(127);
    }
    close(fds[1]);
    FILE *f = fdopen(fds[0], "r");
    char line[8192];
    jit_seq++;
    size_t added = 0;
    while (fgets(line, sizeof(line), f)) {
        /* <id> <level> <state> <signature> [0xblob, 0xcode_begin - 0xcode_end] */
        int cid, level, state;
        char sig[4096];
        unsigned long long blob, cbeg, cend;
        if (sscanf(line, "%d %d %d %4095s [0x%llx, 0x%llx - 0x%llx]", &cid, &level, &state, sig, &blob, &cbeg, &cend) != 7)
            continue;
        if (njit == capjit) {
            capjit = capjit ? capjit * 2 : 4096;
            jit = realloc(jit, capjit * sizeof(*jit));
        }
        jit[njit++] = (jit_entry){blob, cend, jit_seq, jit_name(sig, level)};
        added++;
    }
    fclose(f);
    waitpid(child, NULL, 0);
    qsort(jit, njit, sizeof(*jit), jit_cmp);
    /* Drop exact duplicates (same begin/end) from older snapshots. */
    size_t w = 0;
    for (size_t i = 0; i < njit; i++) {
        if (w > 0 && jit[w - 1].begin == jit[i].begin && jit[w - 1].end == jit[i].end) {
            free(jit[i].name);
            continue;
        }
        jit[w++] = jit[i];
    }
    njit = w;
    (void)added;
}

static const char *jit_lookup(uint64_t pc) {
    /* Find the newest entry containing pc. Entries sorted by begin. */
    size_t lo = 0, hi = njit;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (jit[mid].begin <= pc) lo = mid + 1; else hi = mid;
    }
    const jit_entry *best = NULL;
    for (size_t i = lo; i-- > 0 && i + 64 >= lo;) {
        if (jit[i].begin <= pc && pc < jit[i].end && (!best || jit[i].seq > best->seq)) best = &jit[i];
    }
    return best ? best->name : NULL;
}

typedef struct {
    uint64_t addr;
    char *name;
} native_sym;

static native_sym *natives;
static size_t nnatives;

static int u64cmp(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

static void symbolize_natives(pid_t pid, const char *tmpdir) {
    /* Collect unique non-JIT addresses. */
    size_t cap = 1024, n = 0;
    uint64_t *addrs = malloc(cap * sizeof(*addrs));
    for (size_t i = 0; i < nstacks; i++) {
        for (uint32_t k = 0; k < stacks[i].nframes; k++) {
            uint64_t a = stacks[i].frames[k];
            if (jit_lookup(a)) continue;
            if (n == cap) addrs = realloc(addrs, (cap *= 2) * sizeof(*addrs));
            addrs[n++] = a;
        }
    }
    qsort(addrs, n, sizeof(*addrs), u64cmp);
    size_t u = 0;
    for (size_t i = 0; i < n; i++)
        if (u == 0 || addrs[u - 1] != addrs[i]) addrs[u++] = addrs[i];
    n = u;
    natives = calloc(n, sizeof(*natives));
    nnatives = n;
    for (size_t i = 0; i < n; i++) natives[i].addr = addrs[i];

    char inpath[1024];
    snprintf(inpath, sizeof(inpath), "%s/kperf_atos_in.%d", tmpdir, getpid());
    FILE *in = fopen(inpath, "w");
    for (size_t i = 0; i < n; i++) fprintf(in, "0x%llx\n", (unsigned long long)addrs[i]);
    fclose(in);

    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "atos -p %d -f %s 2>/dev/null", pid, inpath);
    FILE *out = popen(cmd, "r");
    char line[8192];
    size_t i = 0;
    while (out && i < n && fgets(line, sizeof(line), out)) {
        line[strcspn(line, "\n")] = 0;
        char *in_lib = strstr(line, " (in ");
        if (strncmp(line, "0x", 2) == 0 || !in_lib) {
            /* Unresolved; fall back to the mapped file name + offset. */
            char path[PROC_PIDPATHINFO_MAXSIZE];
            struct proc_regionwithpathinfo rp;
            if (proc_pidinfo(pid, PROC_PIDREGIONPATHINFO, addrs[i], &rp, sizeof(rp)) == sizeof(rp) && rp.prp_vip.vip_path[0]) {
                const char *base = strrchr(rp.prp_vip.vip_path, '/');
                snprintf(path, sizeof(path), "%s+0x%llx", base ? base + 1 : rp.prp_vip.vip_path,
                         (unsigned long long)(addrs[i] - rp.prp_prinfo.pri_address));
                natives[i].name = strdup(path);
            } else {
                natives[i].name = strdup("[unknown]");
            }
        } else {
            *in_lib = 0;
            natives[i].name = strdup(line);
        }
        i++;
    }
    if (out) pclose(out);
    for (; i < n; i++) natives[i].name = strdup("[unknown]");
    unlink(inpath);
    free(addrs);
}

static const char *native_lookup(uint64_t a) {
    size_t lo = 0, hi = nnatives;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (natives[mid].addr < a) lo = mid + 1; else hi = mid;
    }
    return lo < nnatives && natives[lo].addr == a ? natives[lo].name : "[unknown]";
}

static void sanitize(char *s) {
    for (; *s; s++)
        if (*s == ';' || *s == '\n') *s = '_';
}

static char *thread_name(pid_t pid, uint64_t tid) {
    struct proc_threadinfo ti;
    char buf[128];
    if (proc_pidinfo(pid, PROC_PIDTHREADID64INFO, tid, &ti, sizeof(ti)) == sizeof(ti) && ti.pth_name[0]) {
        snprintf(buf, sizeof(buf), "[%s tid=%llu]", ti.pth_name, (unsigned long long)tid);
    } else {
        snprintf(buf, sizeof(buf), "[tid=%llu]", (unsigned long long)tid);
    }
    char *r = strdup(buf);
    sanitize(r);
    return r;
}

/* --------------------------------------------------------------------- main */

static volatile sig_atomic_t stop;
static void on_signal(int sig) { (void)sig; stop = 1; }

static void list_events(void) {
    kpep_db *db = NULL;
    if (kpep_db_create(NULL, &db)) { fprintf(stderr, "kpep_db_create failed\n"); return; }
    const char *dbname = NULL;
    kpep_db_name(db, &dbname);
    size_t n = 0;
    kpep_db_events_count(db, &n);
    kpep_event **evs = calloc(n, sizeof(*evs));
    kpep_db_events(db, evs, n * sizeof(*evs));
    printf("# kpep db: %s (%zu events)\n", dbname ? dbname : "?", n);
    for (size_t i = 0; i < n; i++) {
        const char *name = NULL, *desc = NULL;
        kpep_event_name(evs[i], &name);
        kpep_event_description(evs[i], &desc);
        printf("%-40s %s\n", name ? name : "?", desc ? desc : "");
    }
    kpep_db_free(db);
}

static void usage(const char *argv0) {
    fprintf(stderr,
            "usage: sudo %s -p <pid> [-e EVENT] [-c PERIOD] [-d SECONDS] [-o out.collapsed]\n"
            "       %s -l            list PMU events for this CPU\n"
            "defaults: -e CORE_ACTIVE_CYCLE -c 1000000 -d 10\n",
            argv0, argv0);
}

static void reset_kpc(void) {
    uint32_t n = kpc_get_counter_count(KPC_CLASS_CONFIGURABLE_MASK);
    uint64_t zeros[KPC_MAX_COUNTERS] = {0};
    uint32_t zacts[KPC_MAX_COUNTERS] = {0};
    (void)n;
    kperf_sample_set(0);
    kperf_reset();
    kpc_set_counting(0);
    kpc_set_thread_counting(0);
    kpc_set_actionid(KPC_CLASS_CONFIGURABLE_MASK, zacts);
    kpc_set_period(KPC_CLASS_CONFIGURABLE_MASK, zeros);
    kpc_set_config(KPC_CLASS_CONFIGURABLE_MASK, zeros);
    kpc_force_all_ctrs_set(0);
}

int main(int argc, char **argv) {
    pid_t pid = 0;
    const char *event = "CORE_ACTIVE_CYCLE";
    uint64_t period = 1000000;
    double duration = 10;
    const char *outpath = NULL;
    bool list = false;

    int opt;
    while ((opt = getopt(argc, argv, "p:e:c:d:o:lh")) != -1) {
        switch (opt) {
            case 'p': pid = atoi(optarg); break;
            case 'e': event = optarg; break;
            case 'c': period = strtoull(optarg, NULL, 0); break;
            case 'd': duration = atof(optarg); break;
            case 'o': outpath = optarg; break;
            case 'l': list = true; break;
            default: usage(argv[0]); return 2;
        }
    }
    if (!load_frameworks()) return 1;
    if (list) { list_events(); return 0; }
    if (pid <= 0) { usage(argv[0]); return 2; }

    int forced = 0;
    if (kpc_force_all_ctrs_get(&forced)) {
        fprintf(stderr, "kpc access denied: run as root\n");
        return 1;
    }

    /* Target info: uid for jcmd, jcmd next to the target's java binary. */
    struct proc_bsdinfo bi;
    if (proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &bi, sizeof(bi)) != sizeof(bi)) {
        fprintf(stderr, "no such process %d\n", pid);
        return 1;
    }
    target_uid = bi.pbi_uid;
    target_gid = bi.pbi_gid;
    char exe[PROC_PIDPATHINFO_MAXSIZE];
    if (proc_pidpath(pid, exe, sizeof(exe)) > 0) {
        char *slash = strrchr(exe, '/');
        if (slash && strcmp(slash + 1, "java") == 0) {
            *slash = 0;
            snprintf(jcmd_path, sizeof(jcmd_path), "%s/jcmd", exe);
            if (access(jcmd_path, X_OK)) jcmd_path[0] = 0;
        }
    }
    if (!jcmd_path[0]) fprintf(stderr, "warning: target is not a java launcher; JIT frames will be unresolved\n");

    /* Build the PMU configuration: one event, configured to PMI every `period`. */
    kpep_db *db = NULL;
    kpep_config *cfg = NULL;
    kpep_event *ev = NULL;
    int r;
    if ((r = kpep_db_create(NULL, &db))) { fprintf(stderr, "kpep_db_create: %d\n", r); return 1; }
    if ((r = kpep_config_create(db, &cfg))) { fprintf(stderr, "kpep_config_create: %d\n", r); return 1; }
    if ((r = kpep_config_force_counters(cfg))) { fprintf(stderr, "kpep_config_force_counters: %d\n", r); return 1; }
    if ((r = kpep_db_event(db, event, &ev))) { fprintf(stderr, "unknown event %s (try -l): %d\n", event, r); return 1; }
    uint32_t err = 0;
    if ((r = kpep_config_add_event_trigger(cfg, &ev, 0, period, &err))) {
        fprintf(stderr, "kpep_config_add_event_trigger: %d (%u)\n", r, err);
        return 1;
    }
    uint32_t classes = 0;
    kpep_config_kpc_classes(cfg, &classes);
    uint32_t nconfig = kpc_get_config_count(KPC_CLASS_CONFIGURABLE_MASK);
    uint32_t ncounters = kpc_get_counter_count(KPC_CLASS_CONFIGURABLE_MASK);
    kpc_config_t regs[KPC_MAX_COUNTERS] = {0};
    uint64_t periods[KPC_MAX_COUNTERS] = {0};
    size_t map[KPC_MAX_COUNTERS] = {0};
    if ((r = kpep_config_kpc(cfg, regs, sizeof(regs)))) { fprintf(stderr, "kpep_config_kpc: %d\n", r); return 1; }
    if ((r = kpep_config_kpc_periods(cfg, periods, sizeof(periods)))) { fprintf(stderr, "kpep_config_kpc_periods: %d\n", r); return 1; }
    kpep_config_kpc_map(cfg, map, sizeof(map));
    fprintf(stderr, "event %s period %llu: classes=0x%x nconfig=%u ncounters=%u counter=%zu\n", event,
            (unsigned long long)period, classes, nconfig, ncounters, map[0]);
    for (uint32_t i = 0; i < nconfig; i++)
        if (regs[i] || periods[i])
            fprintf(stderr, "  config[%u] = 0x%llx period %llu\n", i, (unsigned long long)regs[i], (unsigned long long)periods[i]);

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    snapshot_codelist(pid);
    fprintf(stderr, "jit: %zu nmethods from %s\n", njit, jcmd_path[0] ? jcmd_path : "-");

    /* Program the PMU and bind PMI -> kperf action 1 (user stack, pid-filtered). */
    uint32_t actions[KPC_MAX_COUNTERS];
    for (uint32_t i = 0; i < KPC_MAX_COUNTERS; i++) actions[i] = periods[i] ? 1 : 0;
    if ((r = kpc_force_all_ctrs_set(1))) { perror("kpc_force_all_ctrs_set"); goto fail; }
    if ((r = kpc_set_config(KPC_CLASS_CONFIGURABLE_MASK, regs))) { perror("kpc_set_config"); goto fail; }
    if ((r = kpc_set_period(KPC_CLASS_CONFIGURABLE_MASK, periods))) { perror("kpc_set_period"); goto fail; }
    if ((r = kpc_set_actionid(KPC_CLASS_CONFIGURABLE_MASK, actions))) { perror("kpc_set_actionid"); goto fail; }
    /* kperf cannot shrink the action array; actions may already exist. */
    kperf_sample_set(0);
    kperf_reset();
    uint32_t nactions = 0;
    kperf_action_count_get(&nactions);
    if ((r = kperf_action_count_set(nactions > 1 ? nactions : 1))) {
        fprintf(stderr, "kperf_action_count_set(%u): %s\n", nactions, strerror(errno));
        goto fail;
    }
    if ((r = kperf_action_samplers_set(1, KPERF_SAMPLER_USTACK | KPERF_SAMPLER_TH_INFO))) { perror("kperf_action_samplers_set"); goto fail; }
    if ((r = kperf_action_filter_set_by_pid(1, pid))) { perror("kperf_action_filter_set_by_pid"); goto fail; }
    if (kdebug_setup(1 << 20)) { perror("kdebug setup (is Instruments/ktrace running?)"); goto fail; }
    if ((r = kperf_sample_set(1))) { perror("kperf_sample_set"); goto fail; }
    if ((r = kpc_set_counting(KPC_CLASS_FIXED_MASK | KPC_CLASS_CONFIGURABLE_MASK))) { perror("kpc_set_counting"); goto fail; }

    fprintf(stderr, "sampling pid %d for %.1fs...\n", pid, duration);
    size_t bufcount = 1 << 18;
    kd_buf *buf = malloc(bufcount * sizeof(kd_buf));
    struct timespec t0, now;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    double last_snap = 0;
    uint64_t nevents = 0;
    while (!stop) {
        clock_gettime(CLOCK_MONOTONIC, &now);
        double el = (now.tv_sec - t0.tv_sec) + (now.tv_nsec - t0.tv_nsec) / 1e9;
        if (el >= duration) break;
        if (kill(pid, 0) && errno == ESRCH) { fprintf(stderr, "target exited\n"); break; }
        size_t len = bufcount * sizeof(kd_buf);
        int mib[3] = {CTL_KERN, KERN_KDEBUG, KERN_KDREADTR};
        if (sysctl(mib, 3, buf, &len, NULL, 0)) { perror("KERN_KDREADTR"); break; }
        /* KDREADTR returns the number of events in len. */
        for (size_t i = 0; i < len; i++) handle_event(&buf[i]);
        nevents += len;
        if (el - last_snap >= 2.0) {
            snapshot_codelist(pid);
            last_snap = el;
        }
        if (len < bufcount / 2) usleep(10000);
    }
    /* Drain. */
    kperf_sample_set(0);
    {
        size_t len = bufcount * sizeof(kd_buf);
        int mib[3] = {CTL_KERN, KERN_KDEBUG, KERN_KDREADTR};
        if (!sysctl(mib, 3, buf, &len, NULL, 0))
            for (size_t i = 0; i < len; i++) handle_event(&buf[i]);
        nevents += len;
    }
    kdebug_teardown();
    reset_kpc();
    free(buf);

    fprintf(stderr, "kdebug events %llu, PMIs %llu (pc captured %llu), stacks %llu, truncated %llu, "
                    "errors %llu, partial %llu, bad hdr %llu, unique stacks %zu\n",
            (unsigned long long)nevents, (unsigned long long)stats.pmis, (unsigned long long)stats.pmis_pc_captured,
            (unsigned long long)stats.stacks, (unsigned long long)stats.truncated, (unsigned long long)stats.errors,
            (unsigned long long)stats.dropped_partial, (unsigned long long)stats.bad_hdr, nstacks);

    /* Symbolize and write collapsed stacks (root first). */
    snapshot_codelist(pid);
    const char *tmpdir = getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp";
    symbolize_natives(pid, tmpdir);

    FILE *out = outpath ? fopen(outpath, "w") : stdout;
    if (!out) { perror(outpath); return 1; }
    uint64_t *tids = NULL;
    char **tnames = NULL;
    size_t ntids = 0;
    uint64_t jit_frames = 0, native_frames = 0, unknown_frames = 0;
    for (size_t i = 0; i < nstacks; i++) {
        stack_t_ *s = &stacks[i];
        const char *tn = NULL;
        for (size_t k = 0; k < ntids; k++)
            if (tids[k] == s->tid) tn = tnames[k];
        if (!tn) {
            tids = realloc(tids, (ntids + 1) * sizeof(*tids));
            tnames = realloc(tnames, (ntids + 1) * sizeof(*tnames));
            tids[ntids] = s->tid;
            tn = tnames[ntids++] = thread_name(pid, s->tid);
        }
        fputs(tn, out);
        for (uint32_t k = s->nframes; k-- > 0;) {
            uint64_t a = s->frames[k];
            const char *name = jit_lookup(a);
            if (name) jit_frames += s->count;
            else {
                name = native_lookup(a);
                if (strcmp(name, "[unknown]")) native_frames += s->count; else unknown_frames += s->count;
            }
            char tmp[1024];
            snprintf(tmp, sizeof(tmp), "%s", name);
            sanitize(tmp);
            fprintf(out, ";%s", tmp);
        }
        fprintf(out, " %llu\n", (unsigned long long)s->count);
    }
    if (out != stdout) fclose(out);
    fprintf(stderr, "frames (weighted): jit %llu, native %llu, unknown %llu\n", (unsigned long long)jit_frames,
            (unsigned long long)native_frames, (unsigned long long)unknown_frames);
    kpep_config_free(cfg);
    kpep_db_free(db);
    return 0;

fail:
    kperf_sample_set(0);
    kdebug_teardown();
    reset_kpc();
    return 1;
}
