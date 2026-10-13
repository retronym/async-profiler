/*
 * Copyright The async-profiler authors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _STACKWALKER_H
#define _STACKWALKER_H

#include <stdint.h>
#include "arguments.h"
#include "event.h"
#include "vmEntry.h"


class JavaFrameAnchor;

class StackWalker {
  public:
    static int walkFP(void* ucontext, const void** callchain, int max_depth);
    static int walkDwarf(void* ucontext, const void** callchain, int max_depth);
    static int walkVM(void* ucontext, ASGCT_CallFrame* frames, int max_depth, int lock_index,
                      StackWalkFeatures features, EventType event_type);
    static int resolveRaw(const uint64_t* pcs, int num_pcs, uint64_t fixup, uint64_t* precise_pc,
                          ASGCT_CallFrame* frames, int max_depth);

    static void checkFault();
};

#endif // _STACKWALKER_H
