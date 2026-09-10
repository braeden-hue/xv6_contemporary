#pragma once
// FCFS scheduler policy -- Phase 1's first (simplest) policy. See plan.md
// Phase 1. Only defines the policy struct; kernel/scheduler.cpp owns
// dispatch()/scheduler_dispatch() and picks the active policy.

extern "C" {
#include "types.h"
#include "proc.h"
}

#include "sched_policy.hpp"

// Picks the RUNNABLE proc with the smallest arrival_seq (kernel/proc.c's
// mark_runnable() stamps this whenever a proc becomes RUNNABLE) -- i.e.
// whoever has been waiting longest goes next. Stateless: reads only fields
// already on struct proc, keeps nothing of its own.
struct FCFS {
    struct proc* pick_next(struct proc* procs, int n) {
        struct proc* candidate = nullptr;
        for (int i = 0; i < n; i++) {
            struct proc* p = &procs[i];
            if (p->state == RUNNABLE) {
                if (!candidate || p->arrival_seq < candidate->arrival_seq)
                    candidate = p;
            }
        }
        return candidate;
    }
};
