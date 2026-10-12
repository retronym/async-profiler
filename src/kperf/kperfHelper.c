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
 * Usage: sudo asprof-kperf -s <socket path> [-u <uid>]   serve any number of agents
 *        sudo asprof-kperf -s <socket path> -p <pid>    serve exactly one session for <pid>
 *
 * The helper only ever samples the pid of the connected socket peer. In the
 * one-shot mode used by asprof, it also rejects peers other than <pid>, and
 * returns as soon as the socket is ready, continuing in the background.
 */

#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <errno.h>
#include <getopt.h>
#include <libgen.h>
#include <limits.h>
#include <libproc.h>
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

// Layout of kperfdata's event descriptor (stable since at least macOS 11)
typedef struct {
    const char* name;
    const char* description;
    const char* errata;
    const char* alias;
    const char* fallback;
    uint32_t mask;  // counters the event may be programmed on, by absolute PMC index
    uint8_t number;
    uint8_t umask;
    uint8_t reserved;
    uint8_t is_fixed;
} kpep_event;

static int (*kpc_force_all_ctrs_set)(int);
static int (*kpc_force_all_ctrs_get)(int*);
static int (*kpc_set_counting)(uint32_t);
static int (*kpc_set_thread_counting)(uint32_t);
static int (*kpc_set_config)(uint32_t, uint64_t*);
static int (*kpc_set_period)(uint32_t, uint64_t*);
static int (*kpc_set_actionid)(uint32_t, uint32_t*);
static uint32_t (*kpc_get_counter_count)(uint32_t);
static uint32_t (*kpc_get_config_count)(uint32_t);
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
static int (*kpep_config_add_event)(kpep_config*, kpep_event**, uint32_t, uint32_t*);
static int (*kpep_config_kpc)(kpep_config*, uint64_t*, size_t);

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
    LOAD(kperf, kpc_set_actionid) LOAD(kperf, kpc_get_counter_count) LOAD(kperf, kpc_get_config_count)
    LOAD(kperf, kperf_action_count_set) LOAD(kperf, kperf_action_count_get)
    LOAD(kperf, kperf_action_samplers_set) LOAD(kperf, kperf_action_filter_set_by_pid)
    LOAD(kperf, kperf_sample_set) LOAD(kperf, kperf_reset)
    LOAD(kpep, kpep_db_create) LOAD(kpep, kpep_db_free) LOAD(kpep, kpep_db_event) LOAD(kpep, kpep_config_create)
    LOAD(kpep, kpep_config_free) LOAD(kpep, kpep_config_force_counters) LOAD(kpep, kpep_config_add_event)
    LOAD(kpep, kpep_config_kpc)
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

#define KDBG_CLASSTYPE  0x10000
#define DBG_PERF        37
#define KDBG_EVENTID(c, s, code) (((uint32_t)(c) << 24) | ((uint32_t)(s) << 16) | ((uint32_t)(code) << 2))
#define DEBUGID_MASK    (~3u)
#define DBG_FUNC_START  1u
#define PERF_GEN_EVENT  KDBG_EVENTID(DBG_PERF, 0, 0)
#define PERF_CS_UDATA   KDBG_EVENTID(DBG_PERF, 2, 4)
#define PERF_CS_UHDR    KDBG_EVENTID(DBG_PERF, 2, 6)
#define PERF_KPC_HNDLR  KDBG_EVENTID(DBG_PERF, 6, 0)
#define CALLSTACK_VALID (1u << 0)
#define SAMPLE_FLAG_PEND_USER (1u << 0)
#define KPC_DESC_COUNTER(desc) (((desc) >> 32) & 0xffff)
#define KPC_DESC_FLAGS(desc)   ((desc) >> 48)
#define KPC_KERNEL_PC          0x01
#define KPC_CAPTURED_PC        0x08

// arm64 kpc config word: counting modes (XNU osfmk/arm64/kpc.c). All zero means all modes.
#define CFGWORD_EL0A64EN_MASK  0x20000
#define CFGWORD_ALLMODES_MASK  0xf0000

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

// --------------------------------------------------------- exclusivity

enum { KTRACE_STATE_OFF = 0, KTRACE_STATE_FG = 1, KTRACE_STATE_BG = 2 };

// The kernel arbitrates ktrace (kdebug + kperf) ownership and the forcing of
// all PMCs, but reports conflicts only as EBUSY/EACCES from whichever call
// hits them first. Check up front and say who holds what.
static bool checkExclusive(char* msg, size_t msglen) {
    int forced = 0;
    if (kpc_force_all_ctrs_get(&forced) == 0 && forced) {
        snprintf(msg, msglen, "PMU counters are reserved by another process (Instruments, xctrace or another profiler?)");
        return false;
    }

    int state = 0, owner = 0;
    size_t len = sizeof(state);
    sysctlbyname("ktrace.state", &state, &len, NULL, 0);
    len = sizeof(owner);
    sysctlbyname("ktrace.owning_pid", &owner, &len, NULL, 0);
    char by[64] = "";
    len = sizeof(by);
    sysctlbyname("ktrace.configured_by", by, &len, NULL, 0);

    if (state == KTRACE_STATE_FG && owner != 0 && owner != getpid() && kill(owner, 0) == 0) {
        snprintf(msg, msglen, "Kernel tracing is in use by %s (pid %d)", by[0] ? by : "another process", owner);
        return false;
    }
    if (state == KTRACE_STATE_BG) {
        // e.g. tailspind: taking over is allowed and it is notified when we reset
        fprintf(stderr, "asprof-kperf: taking over kernel tracing from background tool %s\n", by);
    }
    return true;
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

// Absolute PMC -> index into the request's events, or -1
static int counter_event[KPC_MAX_COUNTERS];

// Which PMCs can latch the exact PC of an event is a property of the event:
// kperfdata's database has it as "pc_capture_counters_mask" (0xe0, i.e. PMC 5-7,
// for the L1D/TLB miss, load source and branch mispredict events on Apple
// silicon; nothing for cycles or instructions), but its event struct does not
// expose it, so read the database plist for this CPU directly.
static CFDictionaryRef kpep_events;

static void loadCaptureMasks(void) {
    int type = 0, subtype = 0;
    uint32_t family = 0;
    size_t len = sizeof(type);
    sysctlbyname("hw.cputype", &type, &len, NULL, 0);
    len = sizeof(subtype);
    sysctlbyname("hw.cpusubtype", &subtype, &len, NULL, 0);
    len = sizeof(family);
    sysctlbyname("hw.cpufamily", &family, &len, NULL, 0);

    char path[128];
    snprintf(path, sizeof(path), "/usr/share/kpep/cpu_%x_%x_%x.plist", type, subtype, family);
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8*)path, strlen(path), false);
    CFReadStreamRef stream = url != NULL ? CFReadStreamCreateWithFile(NULL, url) : NULL;
    if (stream != NULL && CFReadStreamOpen(stream)) {
        CFPropertyListRef plist = CFPropertyListCreateWithStream(NULL, stream, 0, kCFPropertyListImmutable, NULL, NULL);
        if (plist != NULL && CFGetTypeID(plist) == CFDictionaryGetTypeID()) {
            CFDictionaryRef sys = CFDictionaryGetValue(plist, CFSTR("system"));
            CFDictionaryRef cpu = sys != NULL ? CFDictionaryGetValue(sys, CFSTR("cpu")) : NULL;
            kpep_events = cpu != NULL ? CFDictionaryGetValue(cpu, CFSTR("events")) : NULL;
            if (kpep_events != NULL) CFRetain(kpep_events);
        }
        if (plist != NULL) CFRelease(plist);
        CFReadStreamClose(stream);
    }
    if (stream != NULL) CFRelease(stream);
    if (url != NULL) CFRelease(url);
    if (kpep_events == NULL) {
        fprintf(stderr, "asprof-kperf: cannot read %s; precise PC capture disabled\n", path);
    }
}

static uint32_t captureMask(const char* name) {
    if (kpep_events == NULL) return 0;
    CFStringRef key = CFStringCreateWithCString(NULL, name, kCFStringEncodingUTF8);
    CFDictionaryRef ev = CFDictionaryGetValue(kpep_events, key);
    CFRelease(key);
    CFNumberRef mask = ev != NULL ? CFDictionaryGetValue(ev, CFSTR("pc_capture_counters_mask")) : NULL;
    uint32_t v = 0;
    if (mask != NULL) CFNumberGetValue(mask, kCFNumberSInt32Type, &v);
    return v;
}

// Looks up the event's counter mask and its counter-independent config word.
static bool eventConfig(kpep_db* db, const char* name, uint32_t* mask, uint64_t* config) {
    kpep_event* ev = NULL;
    kpep_config* cfg = NULL;
    uint64_t regs[KPC_MAX_COUNTERS] = {0};
    uint32_t err = 0;
    bool ok = kpep_db_event(db, name, &ev) == 0 &&
              kpep_config_create(db, &cfg) == 0 &&
              kpep_config_force_counters(cfg) == 0 &&
              kpep_config_add_event(cfg, &ev, 0, &err) == 0 &&
              kpep_config_kpc(cfg, regs, sizeof(regs)) == 0;
    if (cfg != NULL) kpep_config_free(cfg);
    if (!ok || ev->is_fixed || regs[0] == 0) return false;
    *mask = ev->mask;
    *config = regs[0];
    return true;
}

// Programs one configurable PMC per event to raise a PMI every `period` events,
// all bound to kperf action 1 which samples the user stack of `pid`. Events go
// on the PC-capturing PMCs first, unless the request opts out.
static int startSampling(pid_t pid, const struct KperfRequest* req, struct KperfResponse* resp) {
    char* msg = resp->message;
    size_t msglen = sizeof(resp->message);
    kpep_db* db = NULL;
    uint64_t regs[KPC_MAX_COUNTERS] = {0};
    uint64_t periods[KPC_MAX_COUNTERS] = {0};
    uint32_t actions[KPC_MAX_COUNTERS] = {0};
    int r = EINVAL;

    if (req->num_events == 0 || req->num_events > KPERF_MAX_EVENTS) {
        snprintf(msg, msglen, "Between 1 and %d events are supported", KPERF_MAX_EVENTS);
        return EINVAL;
    }
    if (!checkExclusive(msg, msglen)) {
        return EBUSY;
    }
    if (kpep_db_create(NULL, &db)) {
        snprintf(msg, msglen, "Cannot load PMU event database");
        return EINVAL;
    }

    // Counters are only all available once forced
    if (kpc_force_all_ctrs_set(1)) {
        r = errno ? errno : EACCES;
        snprintf(msg, msglen, "Cannot reserve PMU counters: %s", strerror(r));
        goto out;
    }
    uint32_t nfixed = kpc_get_counter_count(KPC_CLASS_FIXED_MASK);
    uint32_t nconfig = kpc_get_config_count(KPC_CLASS_CONFIGURABLE_MASK);
    if (nconfig == 0 || nfixed + nconfig > KPC_MAX_COUNTERS) {
        snprintf(msg, msglen, "Unexpected PMU layout: %u fixed, %u configurable", nfixed, nconfig);
        goto out;
    }

    uint32_t masks[KPERF_MAX_EVENTS];
    uint64_t configs[KPERF_MAX_EVENTS];
    for (uint32_t i = 0; i < req->num_events; i++) {
        if (!eventConfig(db, req->events[i].name, &masks[i], &configs[i])) {
            snprintf(msg, msglen, "Unknown or fixed-counter-only PMU event %s", req->events[i].name);
            goto out;
        }
        if (req->events[i].period == 0) {
            snprintf(msg, msglen, "Zero period for %s", req->events[i].name);
            goto out;
        }
    }

    // Assign absolute PMCs in three passes:
    //  0. events that support PC capture, on a PMC that captures for them
    //  1. other events, on PMCs that no event in this request could capture on
    //  2. whatever is left, on any allowed PMC
    uint32_t capture_masks[KPERF_MAX_EVENTS];
    uint32_t any_capture = 0;
    bool prefer_capture = !(req->flags & KPERF_REQ_NO_PC_CAPTURE);
    for (uint32_t i = 0; i < req->num_events; i++) {
        capture_masks[i] = prefer_capture ? captureMask(req->events[i].name) & masks[i] : 0;
        any_capture |= capture_masks[i];
    }
    for (int c = 0; c < KPC_MAX_COUNTERS; c++) counter_event[c] = -1;
    uint32_t used = 0;
    for (int pass = 0; pass < 3; pass++) {
        for (uint32_t i = 0; i < req->num_events; i++) {
            if (resp->counter[i] != 0) continue;
            uint32_t allowed = pass == 0 ? capture_masks[i] : pass == 1 ? masks[i] & ~any_capture : masks[i];
            for (uint32_t c = nfixed; c < nfixed + nconfig; c++) {
                if ((used & (1u << c)) || !(allowed & (1u << c))) continue;
                used |= 1u << c;
                resp->counter[i] = (uint8_t)c;
                resp->pc_capture[i] = (capture_masks[i] & (1u << c)) != 0;
                counter_event[c] = (int)i;
                regs[c - nfixed] = configs[i];
                periods[c - nfixed] = req->events[i].period;
                actions[c - nfixed] = 1;
                break;
            }
        }
    }
    if (!(req->flags & KPERF_REQ_KERNEL_COUNTING)) {
        for (uint32_t c = 0; c < nconfig; c++) {
            if (regs[c] != 0) regs[c] = (regs[c] & ~(uint64_t)CFGWORD_ALLMODES_MASK) | CFGWORD_EL0A64EN_MASK;
        }
    }
    for (uint32_t i = 0; i < req->num_events; i++) {
        if (resp->counter[i] == 0) {
            snprintf(msg, msglen, "No free counter for %s", req->events[i].name);
            goto out;
        }
    }

    // kperf cannot shrink its action array, and other clients may have grown it
    uint32_t nactions = 0;
    kperf_sample_set(0);
    kperf_reset();
    kperf_action_count_get(&nactions);

    if (kpc_set_config(KPC_CLASS_CONFIGURABLE_MASK, regs) ||
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
        snprintf(msg, msglen, "Cannot set up kdebug: %s", strerror(r));
        goto out;
    }
    if (kperf_sample_set(1) || kpc_set_counting(KPC_CLASS_FIXED_MASK | KPC_CLASS_CONFIGURABLE_MASK)) {
        r = errno ? errno : EINVAL;
        snprintf(msg, msglen, "Cannot start sampling: %s", strerror(r));
        goto out;
    }
    r = 0;

out:
    kpep_db_free(db);
    if (r != 0) {
        kdebugTeardown();
        resetCounters();
    }
    return r;
}

// --------------------------------------------------------------- serving

// Per-thread state: the PMIs of our pid since the last stack, and the stack being assembled
typedef struct {
    uint64_t tid;
    uint32_t pmis[KPERF_MAX_EVENTS];        // per event, coalesced into the next stack
    uint64_t precise_pc[KPERF_MAX_EVENTS];  // latest captured PC per event
    uint64_t timestamp;
    uint32_t expect;
    uint32_t have;
    uint32_t flags;
    uint64_t frames[KPERF_MAX_FRAMES + 1];
} Pending;

// Per-CPU: the last PMI taken, until kperf confirms it passed the pid filter
typedef struct {
    uint64_t tid;
    uint64_t desc;
    uint64_t pc;
} CpuPmi;

#define MAX_PENDING 1024
#define MAX_CPUS    256
static Pending pending[MAX_PENDING];
static int num_pending;
static CpuPmi cpu_pmi[MAX_CPUS];

static char outbuf[1 << 20];
static size_t outlen;
static int client_fd = -1;

static struct {
    uint64_t pmis, kernel_pmis, stacks, precise;
} stats[KPERF_MAX_EVENTS];
static uint64_t unattributed;

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

static bool emitOne(Pending* p, int event, uint32_t pmis) {
    // The last of the non-async frames is the fixup value appended by
    // callstack_fixup_user(); send it separately.
    uint32_t n = p->expect > 0 ? p->expect - 1 : 0;
    if (n > KPERF_MAX_FRAMES) n = KPERF_MAX_FRAMES;
    struct KperfSample s = {0};
    s.timestamp = p->timestamp;
    s.thread_id = p->tid;
    s.fixup = p->expect > 0 ? p->frames[p->expect - 1] : 0;
    s.precise_pc = p->precise_pc[event];
    s.flags = p->flags;
    s.event = (uint16_t)event;
    s.weight = pmis > 0xffff ? 0xffff : pmis > 0 ? (uint16_t)pmis : 1;
    s.num_frames = n;

    stats[event].stacks++;
    if (s.precise_pc) stats[event].precise++;

    size_t need = sizeof(s) + n * sizeof(uint64_t);
    if (outlen + need > sizeof(outbuf) && !flushOut()) return false;
    memcpy(outbuf + outlen, &s, sizeof(s));
    memcpy(outbuf + outlen + sizeof(s), p->frames, n * sizeof(uint64_t));
    outlen += need;
    return true;
}

// PMIs of several events can coalesce into one stack: send one sample per event,
// each weighted by its own PMI count, sharing the stack
static bool emit(Pending* p) {
    bool any = false, ok = true;
    for (int i = 0; i < KPERF_MAX_EVENTS && ok; i++) {
        if (p->pmis[i] > 0) {
            ok = emitOne(p, i, p->pmis[i]);
            any = true;
        }
    }
    if (!any) {
        unattributed++;
        ok = emitOne(p, 0, 1);
    }
    memset(p->pmis, 0, sizeof(p->pmis));
    memset(p->precise_pc, 0, sizeof(p->precise_pc));
    return ok;
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
    if (id == PERF_KPC_HNDLR && (e->debugid & DBG_FUNC_START)) {
        // PMI on any process: desc, counter value, PC (precise if captured)
        if (e->cpuid < MAX_CPUS) {
            cpu_pmi[e->cpuid] = (CpuPmi){e->arg5, e->arg1, e->arg3};
        }
    } else if (id == PERF_GEN_EVENT && (e->debugid & DBG_FUNC_START) && (e->arg4 & SAMPLE_FLAG_PEND_USER)) {
        // kperf sampled in interrupt context: the PMI passed the pid filter
        if (e->cpuid < MAX_CPUS && cpu_pmi[e->cpuid].tid == e->arg5 && cpu_pmi[e->cpuid].desc != 0) {
            CpuPmi* pmi = &cpu_pmi[e->cpuid];
            Pending* p = pendingFor(e->arg5);
            uint32_t ctr = (uint32_t)KPC_DESC_COUNTER(pmi->desc);
            int event = ctr < KPC_MAX_COUNTERS ? counter_event[ctr] : -1;
            if (event >= 0) {
                p->pmis[event]++;
                p->precise_pc[event] = (KPC_DESC_FLAGS(pmi->desc) & KPC_CAPTURED_PC) ? pmi->pc : 0;
                stats[event].pmis++;
                if (KPC_DESC_FLAGS(pmi->desc) & KPC_KERNEL_PC) stats[event].kernel_pmis++;
            }
            pmi->desc = 0;
        }
    } else if (id == PERF_CS_UHDR) {
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

static bool verbose = true;

static void serveClient(pid_t expected_pid) {
    pid_t pid = 0;
    socklen_t len = sizeof(pid);
    if (getsockopt(client_fd, SOL_LOCAL, LOCAL_PEERPID, &pid, &len) != 0 || pid <= 0) {
        perror("LOCAL_PEERPID");
        return;
    }
    if (expected_pid != 0 && pid != expected_pid) {
        fprintf(stderr, "Rejected connection from pid %d, expected %d\n", pid, expected_pid);
        return;
    }

    struct KperfRequest req;
    if (!readFully(client_fd, &req, sizeof(req)) || req.magic != KPERF_MAGIC) {
        fprintf(stderr, "Bad request from pid %d (agent and helper versions differ?)\n", pid);
        return;
    }
    for (uint32_t i = 0; i < KPERF_MAX_EVENTS; i++) req.events[i].name[KPERF_EVENT_LEN - 1] = 0;

    struct KperfResponse resp;
    memset(&resp, 0, sizeof(resp));
    resp.magic = KPERF_MAGIC;
    resp.error = startSampling(pid, &req, &resp);
    if (write(client_fd, &resp, sizeof(resp)) != sizeof(resp) || resp.error != 0) {
        if (resp.error) fprintf(stderr, "pid %d: %s\n", pid, resp.message);
        else { kdebugTeardown(); resetCounters(); }
        return;
    }
    if (verbose) {
        fprintf(stderr, "Sampling pid %d:", pid);
        for (uint32_t i = 0; i < req.num_events; i++) {
            fprintf(stderr, " %s/%llu on PMC%u%s", req.events[i].name, (unsigned long long)req.events[i].period,
                    resp.counter[i], resp.pc_capture[i] ? " (precise PC)" : "");
        }
        fprintf(stderr, "\n");
    }

    size_t bufcount = 1 << 16;
    kd_buf* buf = malloc(bufcount * sizeof(kd_buf));
    memset(&stats, 0, sizeof(stats));
    unattributed = 0;
    memset(cpu_pmi, 0, sizeof(cpu_pmi));
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
        if (n < bufcount / 2) usleep(5000);
    }
    free(buf);
    kdebugTeardown();
    resetCounters();
    if (verbose) {
        fprintf(stderr, "Stopped sampling pid %d (%s counting, %llu stacks without a PMI):\n", pid,
                (req.flags & KPERF_REQ_KERNEL_COUNTING) ? "user+kernel" : "user-only", (unsigned long long)unattributed);
        for (uint32_t i = 0; i < req.num_events; i++) {
            fprintf(stderr, "  %-32s PMC%u: %llu PMIs (%.1f%% in kernel), %llu stacks, %llu with precise PC (%.1f%%)\n",
                    req.events[i].name, resp.counter[i], (unsigned long long)stats[i].pmis,
                    stats[i].pmis ? 100.0 * stats[i].kernel_pmis / stats[i].pmis : 0.0,
                    (unsigned long long)stats[i].stacks, (unsigned long long)stats[i].precise,
                    stats[i].stacks ? 100.0 * stats[i].precise / stats[i].stacks : 0.0);
        }
    }
}

// Waits for a connection for at most timeout_ms; returns -1 on timeout or error
static int acceptWithTimeout(int s, int timeout_ms) {
    struct pollfd pfd = {s, POLLIN, 0};
    int r;
    while ((r = poll(&pfd, 1, timeout_ms)) < 0 && errno == EINTR && !terminated);
    return r > 0 ? accept(s, NULL, NULL) : -1;
}

int main(int argc, char** argv) {
    const char* path = NULL;
    uid_t uid = getenv("SUDO_UID") ? (uid_t)atoi(getenv("SUDO_UID")) : getuid();
    pid_t target_pid = 0;
    int opt;
    while ((opt = getopt(argc, argv, "s:u:p:")) != -1) {
        switch (opt) {
            case 's': path = optarg; break;
            case 'u': uid = (uid_t)atoi(optarg); break;
            case 'p': target_pid = (pid_t)atoi(optarg); break;
            default: path = NULL; optind = argc; break;
        }
    }
    if (path == NULL) {
        fprintf(stderr, "Usage: sudo %s -s <socket path> [-u <uid> | -p <pid>]\n", argv[0]);
        return 1;
    }
    if (!loadFrameworks()) return 1;
    loadCaptureMasks();
    int forced;
    if (kpc_force_all_ctrs_get(&forced) != 0) {
        fprintf(stderr, "Cannot access PMU: asprof-kperf must run as root\n");
        return 1;
    }

    gid_t gid = (gid_t)-1;
    if (target_pid != 0) {
        struct proc_bsdinfo info;
        if (proc_pidinfo(target_pid, PROC_PIDTBSDINFO, 0, &info, sizeof(info)) != sizeof(info)) {
            fprintf(stderr, "Process %d not found\n", target_pid);
            return 1;
        }
        uid = info.pbi_uid;
        gid = info.pbi_gid;
        verbose = false;
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
    if (chown(path, uid, gid) != 0) perror("chown");

    if (target_pid != 0) {
        // asprof places the socket in a private directory: hand it to the target user too
        char dir[PATH_MAX];
        strncpy(dir, path, sizeof(dir) - 1);
        dir[sizeof(dir) - 1] = 0;
        char* d = dirname(dir);
        if (strcmp(d, "/tmp") != 0 && strcmp(d, "/") != 0 && chown(d, uid, gid) != 0) perror("chown");

        // Detach: the caller continues once the socket is ready.
        // A new session keeps Ctrl-C on the asprof terminal from tearing us down
        // before the agent has stopped.
        pid_t child = fork();
        if (child < 0) {
            perror("fork");
            return 1;
        } else if (child > 0) {
            return 0;
        }
        setsid();

        client_fd = acceptWithTimeout(s, 30000);
        if (client_fd >= 0) {
            serveClient(target_pid);
            close(client_fd);
        } else {
            fprintf(stderr, "asprof-kperf: no connection from pid %d\n", target_pid);
        }
        close(s);
        unlink(path);
        if (strcmp(d, "/tmp") != 0 && strcmp(d, "/") != 0) rmdir(d);
        return 0;
    }

    fprintf(stderr, "asprof-kperf listening on %s for uid %d\n", path, uid);
    while (!terminated) {
        client_fd = accept(s, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            break;
        }
        serveClient(0);
        close(client_fd);
        client_fd = -1;
    }
    close(s);
    unlink(path);
    return 0;
}
