/*
 * Copyright The async-profiler authors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _KPERFEVENTS_H
#define _KPERFEVENTS_H

#ifdef __APPLE__

#include <pthread.h>
#include "engine.h"

// Hardware counter sampling on macOS. PMU interrupts and user stack capture are
// done by the kernel via kperf, driven by the privileged asprof-kperf helper,
// which streams raw stacks over a unix socket. This engine resolves them against
// the live code cache. See research/kperf/DESIGN.md.
class KperfEvents : public Engine {
  private:
    static int _fd;
    static pthread_t _thread;
    static long _interval;
    static int _max_depth;
    static const char* _event;
    static char _error[160];

    static void* threadEntry(void* arg);
    static void readLoop();

  public:
    const char* type() {
        return "kperf";
    }

    const char* title() {
        return _event;
    }

    const char* units() {
        return "total";
    }

    Error start(Arguments& args);
    void stop();
};

#endif // __APPLE__

#endif // _KPERFEVENTS_H
