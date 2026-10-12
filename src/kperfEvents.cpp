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
#include "kperfProtocol.h"
#include "log.h"
#include "profiler.h"
#include "stackWalker.h"
#include "tsc.h"


int KperfEvents::_fd = -1;
pthread_t KperfEvents::_thread;
long KperfEvents::_interval;
int KperfEvents::_max_depth;
const char* KperfEvents::_event;
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

Error KperfEvents::start(Arguments& args) {
    if (args._kperf_path == NULL) {
        return Error("kperf helper socket is not specified");
    }

    _event = args._event;
    _interval = args._interval;
    for (size_t i = 0; i < sizeof(kperf_aliases) / sizeof(kperf_aliases[0]); i++) {
        if (strcmp(_event, kperf_aliases[i].alias) == 0) {
            _event = kperf_aliases[i].name;
            if (_interval <= 0) _interval = kperf_aliases[i].default_interval;
            break;
        }
    }
    if (_interval <= 0) _interval = 1000000;
    _max_depth = args._jstackdepth;

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

    KperfRequest req = {KPERF_MAGIC, 0, (uint64_t)_interval, ""};
    strncpy(req.event, _event, sizeof(req.event) - 1);
    KperfResponse resp;
    if (write(_fd, &req, sizeof(req)) != sizeof(req) || !readFully(_fd, &resp, sizeof(resp)) || resp.magic != KPERF_MAGIC) {
        stop();
        return Error("Bad response from asprof-kperf");
    }
    if (resp.error != 0) {
        resp.message[sizeof(resp.message) - 1] = 0;
        snprintf(_error, sizeof(_error), "asprof-kperf: %s", resp.message);
        stop();
        return Error(_error);
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
    u64 samples = 0;

    KperfSample s;
    while (readFully(_fd, &s, sizeof(s))) {
        if (s.num_frames > KPERF_MAX_FRAMES || !readFully(_fd, pcs, s.num_frames * sizeof(uint64_t))) {
            break;
        }
        if (!_enabled) {
            continue;
        }

        int num_frames = StackWalker::resolveRaw(pcs, s.num_frames, s.fixup, frames, max_frames - 8);
        int tid = tids.lookup(s.thread_id);
        ExecutionEvent event(TSC::ticks());
        profiler->recordExternalSample(_interval, tid, PERF_SAMPLE, &event, num_frames, frames);
        samples++;
    }

    free(frames);
    Log::debug("kperf reader stopped after %llu samples", (unsigned long long)samples);
}

#endif // __APPLE__
