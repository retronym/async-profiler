/*
 * Copyright The async-profiler authors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifdef __APPLE__

#include <mach/mach.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <map>
#include "kperfEvents.h"
#include "log.h"
#include "profiler.h"
#include "stackWalker.h"
#include "tsc.h"


int KperfEvents::_fd = -1;
pthread_t KperfEvents::_thread;
int KperfEvents::_max_depth;
int KperfEvents::_num_events;
bool KperfEvents::_jfr;
u64 KperfEvents::_periods[KPERF_MAX_EVENTS];
char KperfEvents::_names[KPERF_MAX_EVENTS][KPERF_EVENT_LEN];
char KperfEvents::_title[KPERF_MAX_EVENTS * KPERF_EVENT_LEN];
char KperfEvents::_error[160];

struct KperfEventAlias {
    const char* alias;
    const char* name;
    long default_interval;
};

// Linux perf event names mapped to Apple silicon kpep events
static const KperfEventAlias kperf_aliases[] = {
    {"cpu",           "CORE_ACTIVE_CYCLE",         1000000},
    {"cycles",        "CORE_ACTIVE_CYCLE",         1000000},
    {"instructions",  "INST_ALL",                  1000000},
    {"cache-misses",  "L1D_CACHE_MISS_LD_NONSPEC", 10000},
    {"branch-misses", "BRANCH_MISPRED_NONSPEC",    10000},
};

// Maps 64-bit kernel thread ids to the mach port names used as tids by OS::threadId()
class ThreadIdMap {
  private:
    std::map<u64, int> _map;

    void refresh() {
        thread_act_array_t threads;
        mach_msg_type_number_t count;
        if (task_threads(mach_task_self(), &threads, &count) != KERN_SUCCESS) {
            return;
        }
        _map.clear();
        for (mach_msg_type_number_t i = 0; i < count; i++) {
            thread_identifier_info_data_t info;
            mach_msg_type_number_t info_count = THREAD_IDENTIFIER_INFO_COUNT;
            if (thread_info(threads[i], THREAD_IDENTIFIER_INFO, (thread_info_t)&info, &info_count) == KERN_SUCCESS) {
                _map[info.thread_id] = (int)threads[i];
            }
            mach_port_deallocate(mach_task_self(), threads[i]);
        }
        vm_deallocate(mach_task_self(), (vm_address_t)threads, count * sizeof(thread_act_t));
    }

  public:
    int lookup(u64 thread_id) {
        std::map<u64, int>::iterator it = _map.find(thread_id);
        if (it == _map.end()) {
            refresh();
            it = _map.find(thread_id);
            if (it == _map.end()) {
                // Thread has already exited; remember to avoid repeated refreshes
                _map[thread_id] = (int)thread_id;
                return (int)thread_id;
            }
        }
        return it->second;
    }
};

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

// spec: EVENT[@PERIOD][+EVENT[@PERIOD]...], where EVENT is an alias or a kpep name.
// `interval` (-i) applies to a single event without an explicit period.
Error KperfEvents::parseEvents(const char* spec, long interval, KperfRequest& req) {
    char buf[KPERF_MAX_EVENTS * KPERF_EVENT_LEN];
    if (strlen(spec) >= sizeof(buf)) {
        return Error("kperf event list is too long");
    }
    strcpy(buf, spec);

    _num_events = 0;
    _title[0] = 0;
    char* save = NULL;
    for (char* tok = strtok_r(buf, "+", &save); tok != NULL; tok = strtok_r(NULL, "+", &save)) {
        if (_num_events == KPERF_MAX_EVENTS) {
            return Error("Too many kperf events");
        }
        long period = 0;
        char* at = strchr(tok, '@');
        if (at != NULL) {
            *at = 0;
            period = atol(at + 1);
            if (period <= 0) return Error("Invalid kperf event period");
        }
        if (strlen(tok) == 0 || strlen(tok) >= KPERF_EVENT_LEN) {
            return Error("Invalid kperf event name");
        }

        const char* name = tok;
        long default_interval = 1000000;
        for (size_t i = 0; i < sizeof(kperf_aliases) / sizeof(kperf_aliases[0]); i++) {
            if (strcmp(tok, kperf_aliases[i].alias) == 0) {
                name = kperf_aliases[i].name;
                default_interval = kperf_aliases[i].default_interval;
                break;
            }
        }

        int i = _num_events++;
        strcpy(_names[i], tok);
        strncpy(req.events[i].name, name, KPERF_EVENT_LEN - 1);
        _periods[i] = period > 0 ? period : default_interval;
        req.events[i].period = _periods[i];

        if (i > 0) strcat(_title, "+");
        strcat(_title, tok);
    }

    if (_num_events == 0) {
        return Error("No kperf events specified");
    }
    if (_num_events == 1 && strchr(spec, '@') == NULL && interval > 0) {
        _periods[0] = req.events[0].period = interval;
    }
    req.num_events = _num_events;
    return Error::OK;
}

Error KperfEvents::start(Arguments& args) {
    if (args._kperf_path == NULL) {
        return Error("kperf helper socket is not specified");
    }

    KperfRequest req;
    memset(&req, 0, sizeof(req));
    req.magic = KPERF_MAGIC;
    if (getenv("KPERF_NO_PC_CAPTURE") != NULL) {
        // Spike-only switch, to compare against skidded attribution
        req.flags |= KPERF_REQ_NO_PC_CAPTURE;
    }
    if (getenv("KPERF_KERNEL") != NULL) {
        // Spike-only switch: count kernel mode too (see KPERF_REQ_KERNEL_COUNTING)
        req.flags |= KPERF_REQ_KERNEL_COUNTING;
    }
    Error error = parseEvents(args._event, args._interval, req);
    if (error) {
        return error;
    }
    _max_depth = args._jstackdepth;
    _jfr = args._output == OUTPUT_JFR;

    struct sockaddr_un sun = {0};
    sun.sun_family = AF_UNIX;
    if (strlen(args._kperf_path) >= sizeof(sun.sun_path)) {
        return Error("kperf helper socket path is too long");
    }
    strcpy(sun.sun_path, args._kperf_path);

    _fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (_fd < 0 || connect(_fd, (struct sockaddr*)&sun, sizeof(sun)) != 0) {
        snprintf(_error, sizeof(_error), "Cannot connect to asprof-kperf at %s: %s", args._kperf_path, strerror(errno));
        stop();
        return Error(_error);
    }

    KperfResponse resp;
    if (write(_fd, &req, sizeof(req)) != sizeof(req) || !readFully(_fd, &resp, sizeof(resp)) || resp.magic != KPERF_MAGIC) {
        stop();
        return Error("Bad response from asprof-kperf (agent and helper versions differ?)");
    }
    if (resp.error != 0) {
        resp.message[sizeof(resp.message) - 1] = 0;
        snprintf(_error, sizeof(_error), "asprof-kperf: %s", resp.message);
        stop();
        return Error(_error);
    }
    for (int i = 0; i < _num_events; i++) {
        Log::info("kperf: %s (%s) every %llu on PMC%d%s", _names[i], req.events[i].name,
                  (unsigned long long)_periods[i], resp.counter[i], resp.pc_capture[i] ? ", precise PC" : "");
    }

    if (pthread_create(&_thread, NULL, threadEntry, NULL) != 0) {
        stop();
        return Error("Unable to create kperf reader thread");
    }
    return Error::OK;
}

void KperfEvents::stop() {
    if (_fd >= 0) {
        // Closing the socket stops the helper; the reader thread sees EOF
        shutdown(_fd, SHUT_RDWR);
        if (_thread != NULL) {
            pthread_join(_thread, NULL);
            _thread = NULL;
        }
        close(_fd);
        _fd = -1;
    }
}

void* KperfEvents::threadEntry(void* arg) {
    readLoop();
    return NULL;
}

void KperfEvents::readLoop() {
    Profiler* profiler = Profiler::instance();
    ThreadIdMap tids;
    uint64_t pcs[KPERF_MAX_FRAMES];
    int max_frames = _max_depth + KPERF_MAX_FRAMES + 8;
    ASGCT_CallFrame* frames = (ASGCT_CallFrame*)calloc(max_frames, sizeof(ASGCT_CallFrame));
    u64 samples[KPERF_MAX_EVENTS] = {0}, with_pc[KPERF_MAX_EVENTS] = {0}, used_pc[KPERF_MAX_EVENTS] = {0};

    KperfSample s;
    while (readFully(_fd, &s, sizeof(s))) {
        if (s.num_frames > KPERF_MAX_FRAMES || !readFully(_fd, pcs, s.num_frames * sizeof(uint64_t))) {
            break;
        }
        if (!_enabled) {
            continue;
        }

        uint64_t precise_pc = s.precise_pc;
        int num_frames = StackWalker::resolveRaw(pcs, s.num_frames, s.fixup, &precise_pc, frames, max_frames - 8);

        int event = s.event < _num_events ? s.event : 0;
        samples[event]++;
        if (s.precise_pc != 0) with_pc[event]++;
        if (precise_pc != 0) used_pc[event]++;

        if (_num_events > 1 && !_jfr) {
            // Collapsed and flame graph output have no per-sample event field:
            // a root pseudo-frame keeps the profiles of different events apart
            frames[num_frames].bci = BCI_NATIVE_FRAME;
            frames[num_frames].method_id = (jmethodID)_names[event];
            num_frames++;
        }

        int tid = tids.lookup(s.thread_id);
        u64 value = _periods[event] * s.weight;
        HardwareEvent ev(TSC::ticks(), _names[event], value, precise_pc != 0);
        profiler->recordExternalSample(value, tid, HW_SAMPLE, &ev, num_frames, frames);
    }

    free(frames);
    for (int i = 0; i < _num_events; i++) {
        Log::info("kperf: %s: %llu samples, %llu with precise PC, %llu used as leaf", _names[i],
                  (unsigned long long)samples[i], (unsigned long long)with_pc[i], (unsigned long long)used_pc[i]);
    }
}

#endif // __APPLE__
