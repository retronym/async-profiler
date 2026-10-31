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

#define KPERF_MAGIC      0x6b706632  // "kpf2"
#define KPERF_EVENT_LEN  64
#define KPERF_MAX_EVENTS 4
#define KPERF_MAX_FRAMES 256

struct KperfEventRequest {
    uint64_t period;
    char name[KPERF_EVENT_LEN];  // kpep event name, e.g. L1D_CACHE_MISS_LD_NONSPEC
};

struct KperfRequest {
    uint32_t magic;
    uint32_t num_events;
    uint32_t flags;  // KPERF_REQ_*
    uint32_t reserved;
    struct KperfEventRequest events[KPERF_MAX_EVENTS];
};

// Do not prefer the PC-capturing counters (PMC 5-7 on Apple silicon)
#define KPERF_REQ_NO_PC_CAPTURE  1
// Count in kernel mode too. By default counters count user mode only: otherwise
// the kernel's own work for each sample (PMI handler, AST-time user stack walk)
// runs on the sampled thread and is counted, and attributed to its user stack.
#define KPERF_REQ_KERNEL_COUNTING 2

struct KperfResponse {
    uint32_t magic;
    int32_t error;  // 0 on success, otherwise errno-like code
    char message[120];
    uint8_t counter[KPERF_MAX_EVENTS];     // PMC assigned to each event
    uint8_t pc_capture[KPERF_MAX_EVENTS];  // whether that PMC can capture the exact PC
};

struct KperfSample {
    uint64_t timestamp;   // mach_absolute_time units
    uint64_t thread_id;   // 64-bit thread id, as in pthread_threadid_np
    uint64_t fixup;       // interrupted LR (arm64) or [sp] (x86_64)
    uint64_t precise_pc;  // PC captured by the PMU at the event, or 0
    uint32_t flags;       // XNU CALLSTACK_* flags
    uint16_t event;       // index into KperfRequest.events
    uint16_t weight;      // number of PMIs coalesced into this stack (>= 1)
    uint32_t num_frames;  // followed by num_frames uint64_t addresses, leaf first
    uint32_t reserved;
};

#endif // _KPERFPROTOCOL_H
