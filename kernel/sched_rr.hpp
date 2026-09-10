#pragma once
// RR scheduler policy -- Phase 1's second policy. See plan.md Phase 1.
// Only defines the policy struct; kernel/scheduler.cpp owns
// dispatch()/scheduler_dispatch() and picks the active policy.

extern "C" {
#include "types.h"
#include "proc.h"
}

#include "sched_policy.hpp"

// Unlike FCFS, RR carries its own state (`last`): it has to remember where
// it left off, or it would always re-check from index 0 and starve every
// proc after the first RUNNABLE one it finds.
struct RR {
    int last = -1;
    struct proc* pick_next(struct proc* procs, int n) {
        for (int i = 1; i <= n; i++) {          // <= n: must re-check `last` itself too
            int idx = (last + i) % n;           // %, not / -- wrap around circularly
            struct proc* p = &procs[idx];
            if (p->state == RUNNABLE) {
                last = idx;
                return p;
            }
        }
        return nullptr;
    }
};
