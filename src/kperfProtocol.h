/*
 * Copyright The async-profiler authors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _KPERFPROTOCOL_H
#define _KPERFPROTOCOL_H

#include <stdint.h>

// Wire protocol between the async-profiler agent (KperfEvents engine)
// and the privileged asprof-kperf helper on macOS.
//
// agent -> helper: KperfRequest
// helper -> agent: KperfResponse, then a stream of KperfSample records
//                  until either side closes the socket.

#define KPERF_MAGIC     0x6b706631  // "kpf1"
#define KPERF_EVENT_LEN 64
#define KPERF_MAX_FRAMES 256

struct KperfRequest {
    uint32_t magic;
    uint32_t reserved;
    uint64_t period;
    char event[KPERF_EVENT_LEN];
};

struct KperfResponse {
    uint32_t magic;
    int32_t error;  // 0 on success, otherwise errno-like code
    char message[120];
};

struct KperfSample {
    uint64_t timestamp;  // mach_absolute_time units
    uint64_t thread_id;  // 64-bit thread id, as in pthread_threadid_np
    uint64_t fixup;      // interrupted LR (arm64) or [sp] (x86_64)
    uint32_t flags;      // XNU CALLSTACK_* flags
    uint32_t num_frames; // followed by num_frames uint64_t addresses, leaf first
};

#endif // _KPERFPROTOCOL_H
