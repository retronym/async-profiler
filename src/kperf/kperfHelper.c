/*
 * Copyright The async-profiler authors
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * asprof-kperf: privileged helper that samples user stacks on PMU overflow
 * (PMI) for one process, using the private kperf/kperfdata frameworks and the
 * kdebug trace buffer, and streams them to the async-profiler agent running in
 * that process. See research/kperf/DESIGN.md.
 *
 * Usage: sudo asprof-kperf -s <socket path> [-u <uid>]
 *
 * The helper only ever samples the pid of the connected socket peer.
 */

#include <dlfcn.h>
#include <errno.h>
#include <getopt.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/un.h>
#include <unistd.h>
#include "../kperfProtocol.h"

#define KPC_CLASS_FIXED_MASK        (1u << 0)
#define KPC_CLASS_CONFIGURABLE_MASK (1u << 1)
#define KPC_MAX_COUNTERS            32
#define KPERF_SAMPLER_TH_INFO       (1u << 0)
#define KPERF_SAMPLER_USTACK        (1u << 3)

typedef struct kpep_db kpep_db;
typedef struct kpep_config kpep_config;
typedef struct kpep_event kpep_event;

static int (*kpc_force_all_ctrs_set)(int);
static int (*kpc_force_all_ctrs_get)(int*);
static int (*kpc_set_counting)(uint32_t);
static int (*kpc_set_thread_counting)(uint32_t);
static int (*kpc_set_config)(uint32_t, uint64_t*);
static int (*kpc_set_period)(uint32_t, uint64_t*);
static int (*kpc_set_actionid)(uint32_t, uint32_t*);
static int (*kperf_action_count_set)(uint32_t);
static int (*kperf_action_count_get)(uint32_t*);
static int (*kperf_action_samplers_set)(uint32_t, uint32_t);
static int (*kperf_action_filter_set_by_pid)(uint32_t, int32_t);
static int (*kperf_sample_set)(uint32_t);
static int (*kperf_reset)(void);
static int (*kpep_db_create)(const char*, kpep_db**);
static void (*kpep_db_free)(kpep_db*);
static int (*kpep_db_event)(kpep_db*, const char*, kpep_event**);
static int (*kpep_config_create)(kpep_db*, kpep_config**);
static void (*kpep_config_free)(kpep_config*);
static int (*kpep_config_force_counters)(kpep_config*);
static int (*kpep_config_add_event_trigger)(kpep_config*, kpep_event**, uint32_t, uint64_t, uint32_t*);
static int (*kpep_config_kpc)(kpep_config*, uint64_t*, size_t);
static int (*kpep_config_kpc_periods)(kpep_config*, uint64_t*, size_t);

#define LOAD(lib, sym) if ((*(void**)&sym = dlsym(lib, #sym)) == NULL) { fprintf(stderr, "Missing %s\n", #sym); return false; }

static bool loadFrameworks(void) {
    void* kperf = dlopen("/System/Library/PrivateFrameworks/kperf.framework/kperf", RTLD_LAZY);
    void* kpep = dlopen("/System/Library/PrivateFrameworks/kperfdata.framework/kperfdata", RTLD_LAZY);
    if (kperf == NULL || kpep == NULL) {
        fprintf(stderr, "Cannot load kperf frameworks: %s\n", dlerror());
        return false;
    }
    LOAD(kperf, kpc_force_all_ctrs_set) LOAD(kperf, kpc_force_all_ctrs_get) LOAD(kperf, kpc_set_counting)
    LOAD(kperf, kpc_set_thread_counting) LOAD(kperf, kpc_set_config) LOAD(kperf, kpc_set_period)
    LOAD(kperf, kpc_set_actionid) LOAD(kperf, kperf_action_count_set) LOAD(kperf, kperf_action_count_get)
    LOAD(kperf, kperf_action_samplers_set) LOAD(kperf, kperf_action_filter_set_by_pid)
    LOAD(kperf, kperf_sample_set) LOAD(kperf, kperf_reset)
    LOAD(kpep, kpep_db_create) LOAD(kpep, kpep_db_free) LOAD(kpep, kpep_db_event) LOAD(kpep, kpep_config_create)
    LOAD(kpep, kpep_config_free) LOAD(kpep, kpep_config_force_counters) LOAD(kpep, kpep_config_add_event_trigger)
    LOAD(kpep, kpep_config_kpc) LOAD(kpep, kpep_config_kpc_periods)
    return true;
}

// ---------------------------------------------------------------- kdebug

typedef struct {
    uint64_t timestamp;
    uint64_t arg1, arg2, arg3, arg4;
    uint64_t arg5;  // thread id
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
#define DEBUGID_MASK   (~3u)
#define PERF_CS_UDATA  KDBG_EVENTID(DBG_PERF, 2, 4)
#define PERF_CS_UHDR   KDBG_EVENTID(DBG_PERF, 2, 6)
#define CALLSTACK_VALID (1u << 0)

static int kdCtl(int op, int arg, void* buf, size_t* len) {
    int mib[4] = {CTL_KERN, KERN_KDEBUG, op, arg};
    return sysctl(mib, arg >= 0 ? 4 : 3, buf, len, NULL, 0);
}

static int kdebugSetup(int nbufs) {
    size_t z = 0;
    kdCtl(KERN_KDREMOVE, -1, NULL, &z);
    if (kdCtl(KERN_KDSETBUF, nbufs, NULL, &z) || kdCtl(KERN_KDSETUP, -1, NULL, &z)) return -1;
    kd_regtype kdr = {KDBG_CLASSTYPE, DBG_PERF, DBG_PERF + 1, 0, 0};
    size_t len = sizeof(kdr);
    if (kdCtl(KERN_KDSETREG, -1, &kdr, &len)) return -1;
    return kdCtl(KERN_KDENABLE, 1, NULL, &z);
}

static void kdebugTeardown(void) {
    size_t z = 0;
    kdCtl(KERN_KDENABLE, 0, NULL, &z);
    kdCtl(KERN_KDREMOVE, -1, NULL, &z);
}

// ------------------------------------------------------------ PMU setup

static void resetCounters(void) {
    uint64_t zeros[KPC_MAX_COUNTERS] = {0};
    uint32_t zero_actions[KPC_MAX_COUNTERS] = {0};
    kperf_sample_set(0);
    kperf_reset();
    kpc_set_counting(0);
    kpc_set_thread_counting(0);
    kpc_set_actionid(KPC_CLASS_CONFIGURABLE_MASK, zero_actions);
    kpc_set_period(KPC_CLASS_CONFIGURABLE_MASK, zeros);
    kpc_set_config(KPC_CLASS_CONFIGURABLE_MASK, zeros);
    kpc_force_all_ctrs_set(0);
}

// Programs one configurable counter to raise a PMI every `period` events,
// bound to kperf action 1 which samples the user stack of `pid`.
static int startSampling(pid_t pid, const char* event, uint64_t period, char* msg, size_t msglen) {
    kpep_db* db = NULL;
    kpep_config* cfg = NULL;
    kpep_event* ev = NULL;
    uint64_t regs[KPC_MAX_COUNTERS] = {0};
    uint64_t periods[KPC_MAX_COUNTERS] = {0};
    uint32_t actions[KPC_MAX_COUNTERS] = {0};
    uint32_t err = 0;
    int r = EINVAL;

    if (kpep_db_create(NULL, &db) || kpep_config_create(db, &cfg) || kpep_config_force_counters(cfg)) {
        snprintf(msg, msglen, "Cannot load PMU event database");
        goto out;
    }
    if (kpep_db_event(db, event, &ev)) {
        snprintf(msg, msglen, "Unknown PMU event %s", event);
        goto out;
    }
    if (kpep_config_add_event_trigger(cfg, &ev, 0, period, &err) ||
        kpep_config_kpc(cfg, regs, sizeof(regs)) ||
        kpep_config_kpc_periods(cfg, periods, sizeof(periods))) {
        snprintf(msg, msglen, "Cannot configure PMU event %s", event);
        goto out;
    }
    for (int i = 0; i < KPC_MAX_COUNTERS; i++) {
        actions[i] = periods[i] != 0 ? 1 : 0;
    }

    // kperf cannot shrink its action array, and other clients may have grown it
    uint32_t nactions = 0;
    kperf_sample_set(0);
    kperf_reset();
    kperf_action_count_get(&nactions);

    if (kpc_force_all_ctrs_set(1) ||
        kpc_set_config(KPC_CLASS_CONFIGURABLE_MASK, regs) ||
        kpc_set_period(KPC_CLASS_CONFIGURABLE_MASK, periods) ||
        kpc_set_actionid(KPC_CLASS_CONFIGURABLE_MASK, actions) ||
        kperf_action_count_set(nactions > 1 ? nactions : 1) ||
        kperf_action_samplers_set(1, KPERF_SAMPLER_USTACK | KPERF_SAMPLER_TH_INFO) ||
        kperf_action_filter_set_by_pid(1, pid)) {
        r = errno ? errno : EINVAL;
        snprintf(msg, msglen, "Cannot program kperf: %s", strerror(r));
        goto out;
    }
    if (kdebugSetup(1 << 20)) {
        r = errno ? errno : EBUSY;
        snprintf(msg, msglen, "Cannot set up kdebug (Instruments or ktrace running?): %s", strerror(r));
        goto out;
    }
    if (kperf_sample_set(1) || kpc_set_counting(KPC_CLASS_FIXED_MASK | KPC_CLASS_CONFIGURABLE_MASK)) {
        r = errno ? errno : EINVAL;
        snprintf(msg, msglen, "Cannot start sampling: %s", strerror(r));
        goto out;
    }
    r = 0;

out:
    if (cfg != NULL) kpep_config_free(cfg);
    if (db != NULL) kpep_db_free(db);
    if (r != 0) {
        kdebugTeardown();
        resetCounters();
    }
    return r;
}

// --------------------------------------------------------------- serving

typedef struct {
    uint64_t tid;
    uint64_t timestamp;
    uint32_t expect;
    uint32_t have;
    uint32_t flags;
    uint64_t frames[KPERF_MAX_FRAMES + 1];
} Pending;

#define MAX_PENDING 1024
static Pending pending[MAX_PENDING];
static int num_pending;

static char outbuf[1 << 20];
static size_t outlen;
static int client_fd = -1;

static bool flushOut(void) {
    size_t off = 0;
    while (off < outlen) {
        ssize_t w = write(client_fd, outbuf + off, outlen - off);
        if (w < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        off += w;
    }
    outlen = 0;
    return true;
}

static bool emit(Pending* p) {
    // The last of the non-async frames is the fixup value appended by
    // callstack_fixup_user(); send it separately.
    uint32_t n = p->expect > 0 ? p->expect - 1 : 0;
    if (n > KPERF_MAX_FRAMES) n = KPERF_MAX_FRAMES;
    struct KperfSample s = {p->timestamp, p->tid, p->expect > 0 ? p->frames[p->expect - 1] : 0, p->flags, n};
    size_t need = sizeof(s) + n * sizeof(uint64_t);
    if (outlen + need > sizeof(outbuf) && !flushOut()) return false;
    memcpy(outbuf + outlen, &s, sizeof(s));
    memcpy(outbuf + outlen + sizeof(s), p->frames, n * sizeof(uint64_t));
    outlen += need;
    return true;
}

static Pending* pendingFor(uint64_t tid) {
    for (int i = 0; i < num_pending; i++) {
        if (pending[i].tid == tid) return &pending[i];
    }
    if (num_pending == MAX_PENDING) num_pending = 0;  // drop everything; should never happen
    Pending* p = &pending[num_pending++];
    memset(p, 0, sizeof(*p));
    p->tid = tid;
    return p;
}

static bool handleEvent(const kd_buf* e) {
    uint32_t id = e->debugid & DEBUGID_MASK;
    if (id == PERF_CS_UHDR) {
        Pending* p = pendingFor(e->arg5);
        p->flags = (uint32_t)e->arg1;
        p->expect = (p->flags & CALLSTACK_VALID) ? (uint32_t)e->arg2 : 0;
        p->have = 0;
        p->timestamp = e->timestamp;
    } else if (id == PERF_CS_UDATA) {
        Pending* p = pendingFor(e->arg5);
        if (p->expect == 0) return true;
        uint64_t a[4] = {e->arg1, e->arg2, e->arg3, e->arg4};
        for (int i = 0; i < 4 && p->have < p->expect; i++, p->have++) {
            if (p->have <= KPERF_MAX_FRAMES) p->frames[p->have] = a[i];
        }
        if (p->have >= p->expect) {
            if (p->expect > KPERF_MAX_FRAMES + 1) {
                // Keep the fixup in the last slot
                p->frames[KPERF_MAX_FRAMES] = 0;
                p->expect = KPERF_MAX_FRAMES + 1;
            }
            bool ok = emit(p);
            p->expect = 0;
            return ok;
        }
    }
    return true;
}

static volatile sig_atomic_t terminated;

static void onSignal(int sig) {
    terminated = 1;
}

static bool clientClosed(void) {
    struct pollfd pfd = {client_fd, POLLIN, 0};
    if (poll(&pfd, 1, 0) > 0) {
        char c;
        return recv(client_fd, &c, 1, MSG_PEEK | MSG_DONTWAIT) <= 0;
    }
    return false;
}

static bool readFully(int fd, void* buf, size_t len) {
    while (len > 0) {
        ssize_t r = read(fd, buf, len);
        if (r <= 0) {
            if (r < 0 && errno == EINTR) continue;
            return false;
        }
        buf = (char*)buf + r;
        len -= r;
    }
    return true;
}

static void serveClient(void) {
    pid_t pid = 0;
    socklen_t len = sizeof(pid);
    if (getsockopt(client_fd, SOL_LOCAL, LOCAL_PEERPID, &pid, &len) != 0 || pid <= 0) {
        perror("LOCAL_PEERPID");
        return;
    }

    struct KperfRequest req;
    if (!readFully(client_fd, &req, sizeof(req)) || req.magic != KPERF_MAGIC) {
        fprintf(stderr, "Bad request from pid %d\n", pid);
        return;
    }
    req.event[KPERF_EVENT_LEN - 1] = 0;

    struct KperfResponse resp = {KPERF_MAGIC, 0, ""};
    resp.error = startSampling(pid, req.event, req.period, resp.message, sizeof(resp.message));
    if (write(client_fd, &resp, sizeof(resp)) != sizeof(resp) || resp.error != 0) {
        if (resp.error) fprintf(stderr, "pid %d: %s\n", pid, resp.message);
        if (resp.error == 0) { kdebugTeardown(); resetCounters(); }
        return;
    }
    fprintf(stderr, "Sampling pid %d: %s every %llu\n", pid, req.event, (unsigned long long)req.period);

    size_t bufcount = 1 << 16;
    kd_buf* buf = malloc(bufcount * sizeof(kd_buf));
    uint64_t samples_before = 0;
    num_pending = 0;
    outlen = 0;
    while (!terminated && !clientClosed()) {
        size_t n = bufcount * sizeof(kd_buf);
        int mib[3] = {CTL_KERN, KERN_KDEBUG, KERN_KDREADTR};
        if (sysctl(mib, 3, buf, &n, NULL, 0) != 0) {
            perror("KERN_KDREADTR");
            break;
        }
        bool ok = true;
        for (size_t i = 0; i < n && ok; i++) {
            ok = handleEvent(&buf[i]);
        }
        if (!ok || !flushOut()) break;
        samples_before += n;
        if (n < bufcount / 2) usleep(5000);
    }
    free(buf);
    kdebugTeardown();
    resetCounters();
    fprintf(stderr, "Stopped sampling pid %d (%llu trace events)\n", pid, (unsigned long long)samples_before);
}

int main(int argc, char** argv) {
    const char* path = NULL;
    uid_t uid = getenv("SUDO_UID") ? (uid_t)atoi(getenv("SUDO_UID")) : getuid();
    int opt;
    while ((opt = getopt(argc, argv, "s:u:")) != -1) {
        switch (opt) {
            case 's': path = optarg; break;
            case 'u': uid = (uid_t)atoi(optarg); break;
            default: path = NULL; optind = argc; break;
        }
    }
    if (path == NULL) {
        fprintf(stderr, "Usage: sudo %s -s <socket path> [-u <uid allowed to connect>]\n", argv[0]);
        return 1;
    }
    if (!loadFrameworks()) return 1;
    int forced;
    if (kpc_force_all_ctrs_get(&forced) != 0) {
        fprintf(stderr, "Cannot access PMU: asprof-kperf must run as root\n");
        return 1;
    }

    signal(SIGPIPE, SIG_IGN);
    struct sigaction sa = {0};
    sa.sa_handler = onSignal;  // no SA_RESTART: interrupt accept()
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un sun = {0};
    sun.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof(sun.sun_path)) {
        fprintf(stderr, "Socket path too long\n");
        return 1;
    }
    strcpy(sun.sun_path, path);
    unlink(path);
    if (bind(s, (struct sockaddr*)&sun, sizeof(sun)) != 0 || listen(s, 1) != 0) {
        perror(path);
        return 1;
    }
    // Only the target user may connect
    chmod(path, 0600);
    if (chown(path, uid, (gid_t)-1) != 0) perror("chown");
    fprintf(stderr, "asprof-kperf listening on %s for uid %d\n", path, uid);

    while (!terminated) {
        client_fd = accept(s, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            break;
        }
        serveClient();
        close(client_fd);
        client_fd = -1;
    }
    close(s);
    unlink(path);
    return 0;
}
