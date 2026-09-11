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
#include "sched_priority.hpp"

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
                // Phase 1.8: leaving RUNNABLE -- mirror image of mark_runnable()'s
                // increment in kernel/proc.c. Unconditional, same precedent as
                // arrival_seq: cheap enough to keep correct regardless of which
                // policy is active, not gated behind the active policy's type.
                if (next->priority == 1)
                    __sync_fetch_and_sub(&g_ls_runnable_count, 1);
                // Phase 1.8b: close the RUNNABLE-wait interval mark_runnable()
                // opened (kernel/proc.c) -- same unconditional/policy-agnostic
                // precedent as the g_ls_runnable_count decrement just above.
                // next->lock is held here, so next->wait_ticks_total/_max need
                // no extra locking (both are only ever written here and read
                // by sys_sched_stats() while holding the same lock). `ticks`
                // itself is read via __atomic_load_n, not tickslock (see
                // mark_runnable()'s comment for why) and not a plain read
                // (Codex audit, 2026-09-12: a bare load races with
                // clockintr()'s atomic increment).
                //
                // The subtraction is done in uint32, not uint64: `ticks` is a
                // 32-bit hardware-tick counter (kernel/trap.c) that can wrap:
                // if it wraps between sched_ready_tick being stamped and now,
                // a naive 64-bit `now - sched_ready_tick` computes a huge
                // wrong value instead of the small true elapsed time. Modular
                // (wrapping) uint32 subtraction gives the correct delta as
                // long as the true elapsed tick count is strictly less than
                // 2^32 (exactly 2^32 wraps back to a computed delta of 0,
                // per Codex's 2nd-round audit, 2026-09-12) -- then it's
                // widened into the uint64 storage
                // field, same technique as Linux's jiffies/time_after().
                {
                    uint32 now32 = (uint32)__atomic_load_n(&ticks, __ATOMIC_RELAXED);
                    uint32 w32 = now32 - (uint32)next->sched_ready_tick;
                    uint64 w = w32;
                    next->wait_ticks_total += w;
                    if (w > next->wait_ticks_max)
                        next->wait_ticks_max = w;
                }
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

// Phase 1.8 step 3: PriorityPreempt<true> run for user/latencytest.c
// comparison. Swap to RR and rebuild for the other side -- this one line
// is the whole difference.
using ActivePolicy = PriorityPreempt<true>;

// File-scope (not function-local): policy_should_preempt() below needs the
// same instance dispatch() is using -- a should_preempt() that carries its
// own state (a future policy might) must see what pick_next() just did.
static ActivePolicy g_policy;

extern "C" [[noreturn]] void scheduler_dispatch(void)
{
    dispatch(g_policy);
}

// Phase 1.8: kernel/trap.c calls this once per timer tick instead of
// unconditionally yield()-ing, so the active policy decides when to give up
// the CPU. int at the boundary, not bool -- this is a C/C++ linkage seam
// (same convention as the RunnableReason boundary planned for CFS).
extern "C" int policy_should_preempt(struct proc* running)
{
    return g_policy.should_preempt(running) ? 1 : 0;
}
