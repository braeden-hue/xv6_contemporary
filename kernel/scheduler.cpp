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
#include "span_observe_config.h"

#if EP2_SPAN_OBSERVE_ENABLED
extern "C" void span_observe_dispatch_enter(struct proc* p);
extern "C" void span_observe_dispatch_exit(struct proc* p, uint64 t_exit);
#endif

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
#if EP2_SPAN_OBSERVE_ENABLED
                span_observe_dispatch_enter(next);
                swtch(&c->context, &next->context);
                span_observe_dispatch_exit(next, r_time());
#else
                swtch(&c->context, &next->context);
#endif
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
using ActivePolicy = PrioritySelectOnly<true>;   // SCHED-EP2-BUDGET-01 S11: baseline fixed to SelectOnly for the span-observation pilot (Budget's selection changes must not be active here)

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

// SCHED-EP2-BUDGET-01 (design/ep2_selectonly_budget/spec.md S2.2): a second,
// unconditional per-tick hook, called from BOTH kernel/trap.c's usertrap()
// AND kerneltrap() -- unlike policy_should_preempt() above, which
// kerneltrap() never calls at all, so charging a policy's tick-budget only
// from that path would silently miss every kernel-mode tick. Concept-gated
// so policies without a charge_tick() method (RR/FCFS/PriorityPreempt/
// PrioritySelectOnly) compile this down to nothing -- zero added
// instructions on their path, same "if constexpr, not a runtime branch"
// discipline as Episode 1's static-specialization findings.
template<typename P>
concept HasChargeTick = requires(P& policy, struct proc* running) {
    policy.charge_tick(running);
};

// `if constexpr`'s "discard the other branch without checking its members
// exist" behavior only applies inside a template -- policy_charge_tick()
// itself is a plain (non-template) extern "C" function, so putting the
// `if constexpr` directly in its body still fully typechecks BOTH branches
// against the one concrete ActivePolicy and fails to compile for any
// policy lacking charge_tick(). Routing through this small template
// wrapper is what actually makes the discarding apply.
template<typename P>
static void charge_tick_impl(P& policy, struct proc* running)
{
    if constexpr (HasChargeTick<P>) {
        policy.charge_tick(running);
    }
}

extern "C" void policy_charge_tick(struct proc* running)
{
    charge_tick_impl(g_policy, running);
}

// Policy-level (not per-pid) diagnostic counters -- kernel/sysproc.c's
// sys_sched_budget_stats() copies these out to user space. Policies without
// get_budget_stats() report all-zero, so the SAME query path works
// identically for RR/SelectOnly/PrioritySelectBudget (design spec S4).
template<typename P>
concept HasBudgetStats = requires(const P& policy, uint64* out) {
    policy.get_budget_stats(out);
};

template<typename P>
static void budget_stats_impl(const P& policy, uint64* out)
{
    if constexpr (HasBudgetStats<P>) {
        policy.get_budget_stats(out);
    } else {
        out[0] = 0;
        out[1] = 0;
        out[2] = 0;
    }
}

extern "C" void policy_budget_stats(uint64* out)
{
    budget_stats_impl(g_policy, out);
}
