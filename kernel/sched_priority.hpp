#pragma once
// Priority-based preemption -- Phase 1.8. Two priority levels declared
// explicitly by the process (kernel/proc.h's `priority` field), never
// inferred from run length. pick_next() always prefers a RUNNABLE
// LatencySensitive proc over any Normal one; should_preempt() makes a
// Normal-priority running proc yield every tick unconditionally (same as
// RR, regardless of whether any LatencySensitive proc is actually waiting),
// while a running LatencySensitive proc is never preempted by this policy.
//
// UseCounter selects how pick_next()'s first pass (the LatencySensitive
// scan) decides whether it's even worth scanning: true reads the O(1)
// global g_ls_runnable_count (maintained in kernel/proc.c's
// mark_runnable() and kernel/scheduler.cpp's dispatch()) and skips the
// scan when it's 0; false always runs the scan regardless, O(n). Both
// builds are worst-case O(n) overall (the fallback RR pass below always
// scans) -- comparing their per-call decision cost (not an O(1)-vs-O(n)
// asymptotic claim) is the experiment. should_preempt() itself does not
// use UseCounter or g_ls_runnable_count at all.

extern "C" {
#include "types.h"
#include "param.h"
#include "proc.h"
extern int g_ls_runnable_count;   // kernel/proc.c
extern uint ticks;                // kernel/trap.c -- SCHED-EP2-BUDGET-01's window tracking
}

#include "sched_policy.hpp"

enum class Priority : int { Normal = 0, LatencySensitive = 1 };

template<bool UseCounter>
struct PriorityPreempt {
    int last = -1;

    struct proc* pick_next(struct proc* procs, int n) {
        // Whether it's even worth scanning for a waiting LatencySensitive proc.
        // UseCounter=true: O(1) read of the maintained counter -- skip the
        // scan below entirely in the common case (nobody latency-sensitive).
        // UseCounter=false: always do the scan, the "dumb" baseline -- this
        // is the added-decision-cost comparison (Phase 1.8's ④).
        bool maybe_ls = UseCounter ? (g_ls_runnable_count > 0) : true;

        if (maybe_ls) {
            for (int i = 1; i <= n; i++) {   // 1st pass: LatencySensitive wins outright
                int idx = (last + i) % n;
                struct proc* p = &procs[idx];
                if (p->state == RUNNABLE && p->priority == (int)Priority::LatencySensitive) {
                    last = idx;
                    return p;
                }
            }
        }
        for (int i = 1; i <= n; i++) {        // 2nd pass: plain RR among Normal procs
            int idx = (last + i) % n;
            struct proc* p = &procs[idx];
            if (p->state == RUNNABLE) {
                last = idx;
                return p;
            }
        }
        return nullptr;
    }

    // Normal procs still yield every tick, exactly like RR -- the priority
    // mechanism's whole effect comes from pick_next()'s reordering above,
    // not from suppressing ordinary time-slicing between Normal peers (that
    // would starve same-priority fairness whenever nobody is
    // LatencySensitive, which is most of the time real xv6 workloads run).
    // LatencySensitive itself is left to run to completion (rule 1: don't
    // interrupt short work if avoidable).
    bool should_preempt(struct proc* running) {
        return running->priority != (int)Priority::LatencySensitive;
    }
};

// Mixed Workload Response-Time Experiment (design/mixed_workload_experiment/, rev5, ACCEPTED
// 2026-09-12): ablation policy isolating *selection* from *protection*. PriorityPreempt above
// changes both at once (LatencySensitive-first reordering in pick_next() AND never-yield
// protection in should_preempt()), so an observed response-time improvement under it can't be
// attributed to either mechanism alone. PrioritySelectOnly keeps the same reordering (delegated
// to a PriorityPreempt<UseCounter> member, not duplicated -- same pick_next() logic and `last`
// state, unmodified) but always yields every tick, exactly like RR/Normal under PriorityPreempt.
// Comparing RR vs PrioritySelectOnly vs PriorityPreempt on the same workload (user/mixbench.c)
// isolates each mechanism's separate contribution.
template<bool UseCounter>
struct PrioritySelectOnly {
    PriorityPreempt<UseCounter> inner;   // reused verbatim, not reimplemented

    struct proc* pick_next(struct proc* procs, int n) {
        return inner.pick_next(procs, n);
    }

    // The only behavioral difference from PriorityPreempt: no non-preemption
    // protection for LatencySensitive. Every proc yields every tick, same as
    // plain RR -- selection reordering is the only effect left active.
    bool should_preempt(struct proc*) {
        return true;
    }
};

// SCHED-EP2-BUDGET-01 (design/ep2_selectonly_budget/spec.md, rev2):
// "SelectOnly의 짧은 작업 응답 개선을 유지하면서, 높은 우선순위 작업이 긴
// 작업과 드라이버를 과도하게 밀어내는 것을 제한할 수 있는가?" -- Budget
// device only (Aging is a separate later experiment).
//
// Pure, deterministic, unit-testable core: takes the current tick as an
// explicit parameter rather than reading the `ticks` global itself, so a
// boot-time self-test can feed synthetic sequences (window boundaries,
// multi-window skips, uint32 wraparound) without needing real elapsed
// time. The caller (PrioritySelectBudget below) supplies the real,
// atomically-loaded `ticks` value.
//
// Fixed origin at tick 0 (not "whenever this tracker was first queried" --
// rev1's bug): window_index = now / WindowTicks, plain unsigned division.
// A transition is detected by "window_index differs from the last one
// observed", which stays correct across a uint32 wrap of `now` itself --
// we never compare index MAGNITUDES, only whether the index changed. The
// one cosmetic exception is the single window straddling the wrap point:
// 2^32 is not a multiple of WindowTicks in general (e.g. WindowTicks=10 ->
// 2^32 mod 10 = 6), so that ONE window is exactly (2^32 mod WindowTicks)
// ticks long, not a full WindowTicks -- this is NOT "exact WindowTicks-size
// windows are maintained all the way to the wrap", just "the transition is
// still detected correctly, with one short/irregular window at the seam".
// Same accepted class of approximation as this codebase's existing
// wait_ticks_max wraparound handling (kernel/scheduler.cpp), and irrelevant
// at this experiment's scale (a few hundred ticks per boot, wrap is at
// 2^32).
template<int BudgetTicks, int WindowTicks>
class BudgetWindowTracker {
    static_assert(BudgetTicks > 0 && BudgetTicks < WindowTicks,
        "0 < BudgetTicks < WindowTicks required -- 0 means the budget is "
        "always exhausted, >=WindowTicks means it's effectively unlimited "
        "(the policy degenerates to SelectOnly)");

    uint32 last_window_index = 0;
    bool have_window = false;
    int charged_this_window = 0;

    uint64 ls_charged_ticks_total = 0;   // diagnostic: never resets across windows
    uint64 budget_exhaustions_total = 0; // counts window-transitions INTO exhausted, once per window

    void observe(uint32 now) {
        uint32 idx = now / (uint32)WindowTicks;
        if (!have_window || idx != last_window_index) {
            last_window_index = idx;
            have_window = true;
            charged_this_window = 0;
        }
    }

public:
    bool budget_available(uint32 now) {
        observe(now);
        return charged_this_window < BudgetTicks;
    }

    // Called once per timer tick that actually hit an LS-priority proc
    // (see PrioritySelectBudget::charge_tick below) -- NOT once per
    // pick_next() selection (rev1's bug).
    void charge(uint32 now) {
        observe(now);
        ls_charged_ticks_total++;
        if (charged_this_window < BudgetTicks) {
            charged_this_window++;
            if (charged_this_window == BudgetTicks) {
                budget_exhaustions_total++;  // this tick is the one that tipped it over
            }
        }
        // already >= BudgetTicks: keep accumulating the diagnostic total
        // above, but charged_this_window itself doesn't need to grow further.
    }

    uint64 ls_charged_ticks() const { return ls_charged_ticks_total; }
    uint64 budget_exhaustions() const { return budget_exhaustions_total; }
};

template<bool UseCounter, int BudgetTicks, int WindowTicks>
struct PrioritySelectBudget {
    int last = -1;
    BudgetWindowTracker<BudgetTicks, WindowTicks> budget;
    uint64 normal_selected_while_ls_runnable = 0;

    struct proc* pick_next(struct proc* procs, int n) {
        uint32 now = (uint32)__atomic_load_n(&ticks, __ATOMIC_RELAXED);
        bool budget_ok = budget.budget_available(now);
        bool maybe_ls = UseCounter ? (g_ls_runnable_count > 0) : true;

        if (maybe_ls && budget_ok) {
            for (int i = 1; i <= n; i++) {
                int idx = (last + i) % n;
                struct proc* p = &procs[idx];
                if (p->state == RUNNABLE && p->priority == (int)Priority::LatencySensitive) {
                    last = idx;
                    return p;
                }
            }
        }

        // Fallback: plain RR among ALL RUNNABLE (budget-exhausted LS procs
        // are equal to Normal here). If the budget was unavailable AND
        // there was an actual LS proc runnable at this decision point,
        // whichever proc we pick here that turns out to be Normal is a
        // genuine "displacement prevented" event -- distinct from just
        // "the budget counter happened to be at zero" (design spec S4).
        bool ls_was_waiting = !budget_ok && (g_ls_runnable_count > 0);
        for (int i = 1; i <= n; i++) {
            int idx = (last + i) % n;
            struct proc* p = &procs[idx];
            if (p->state == RUNNABLE) {
                last = idx;
                if (ls_was_waiting && p->priority != (int)Priority::LatencySensitive) {
                    normal_selected_while_ls_runnable++;
                }
                return p;
            }
        }
        return nullptr;
    }

    // Called from BOTH kernel/trap.c's usertrap() and kerneltrap() on
    // every timer tick, unconditionally -- NOT just from the
    // should_preempt() path, which kerneltrap() never calls at all (design
    // spec S2.1). Charges exactly one tick if the proc the timer actually
    // hit was LatencySensitive; a no-op otherwise.
    void charge_tick(struct proc* running) {
        if (running && running->priority == (int)Priority::LatencySensitive) {
            uint32 now = (uint32)__atomic_load_n(&ticks, __ATOMIC_RELAXED);
            budget.charge(now);
        }
    }

    bool should_preempt(struct proc*) { return true; }   // SelectOnly-style, not Preempt-style

    void get_budget_stats(uint64 out[3]) const {
        out[0] = budget.ls_charged_ticks();
        out[1] = budget.budget_exhaustions();
        out[2] = normal_selected_while_ls_runnable;
    }
};
