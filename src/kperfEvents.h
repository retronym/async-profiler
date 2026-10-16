/*
 * Copyright The async-profiler authors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _KPERFEVENTS_H
#define _KPERFEVENTS_H

#ifdef __APPLE__

#include <pthread.h>
#include "arch.h"
#include "engine.h"
#include "kperfProtocol.h"

// Hardware counter sampling on macOS. PMU interrupts and user stack capture are
// done by the kernel via kperf, driven by the privileged asprof-kperf helper,
// which streams raw stacks over a unix socket. This engine resolves them against
// the live code cache. See research/kperf/DESIGN.md.
class KperfEvents : public Engine {
  private:
    static int _fd;
    static pthread_t _thread;
    static int _max_depth;
    static int _num_events;
    static bool _jfr;
    static u64 _periods[KPERF_MAX_EVENTS];
    static char _names[KPERF_MAX_EVENTS][KPERF_EVENT_LEN];  // as requested, e.g. cache-misses
    static char _title[KPERF_MAX_EVENTS * KPERF_EVENT_LEN];
    static char _error[160];

    static Error parseEvents(const char* spec, long interval, KperfRequest& req);

    static void* threadEntry(void* arg);
    static void readLoop();

  public:
    const char* type() {
        return "kperf";
    }

    const char* title() {
        return _title;
    }

    const char* units() {
        return "total";
    }

    Error start(Arguments& args);
    void stop();
};

#endif // __APPLE__

#endif // _KPERFEVENTS_H
