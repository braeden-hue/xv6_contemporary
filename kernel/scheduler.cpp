// Phase 1: owns the policy-agnostic dispatch mechanics (locking, the
// RUNNABLE->RUNNING transition, swtch) and picks which policy struct is
// actually wired up. Policy structs themselves live in sched_*.hpp so more
// than one can be included here at once without a link-time symbol clash --
// swapping policies is the one `using ActivePolicy = ...` line below.

extern "C" {
#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "proc.h"
#include "defs.h"
}

#include "sched_policy.hpp"
#include "sched_fcfs.hpp"
#include "sched_rr.hpp"

template<SchedulerPolicy P>
[[noreturn]] static void dispatch(P& policy)
{
    struct cpu* c = mycpu();
    c->proc = 0;
    for (;;) {
        intr_on();
        intr_off();
        struct proc* next = policy.pick_next(proc, NPROC);
        if (next) {
            acquire(&next->lock);
            if (next->state == RUNNABLE) {
                next->state = RUNNING;
                c->proc = next;
                swtch(&c->context, &next->context);
                c->proc = 0;
            }
            release(&next->lock);
        } else {
            asm volatile("wfi");
        }
    }
}

using ActivePolicy = RR;   // swap to FCFS (or later CFS) here, then rebuild

extern "C" [[noreturn]] void scheduler_dispatch(void)
{
    static ActivePolicy policy;
    dispatch(policy);
}
