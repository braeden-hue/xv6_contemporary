// SCHED-EP2-BUDGET-01 (design/ep2_selectonly_budget/spec.md S7 step 2):
// deterministic, boot-time self-test of BudgetWindowTracker using SYNTHETIC
// tick values (never the real `ticks` global) -- window boundary, reset,
// multi-window skip, and uint32 wraparound are all exercised without
// needing real elapsed time. This is a correctness gate for the tracker
// itself, run before the tracker is ever wired into a real policy's
// pick_next()/charge_tick() against the real clock.
extern "C" {
#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
extern int g_ls_runnable_count;
extern uint ticks;
#include "defs.h"
}

#include "sched_priority.hpp"   // BudgetWindowTracker<BudgetTicks, WindowTicks>

static int g_budget_window_fail_count = 0;

static void check(const char* label, bool got, bool want) {
    bool ok = (got == want);
    printf((char*)"[budget-window-test] %s got=%d want=%d %s\n",
           label, (int)got, (int)want, ok ? (char*)"PASS" : (char*)"FAIL");
    if (!ok) g_budget_window_fail_count++;
}

static void check_u64(const char* label, uint64 got, uint64 want) {
    bool ok = (got == want);
    printf((char*)"[budget-window-test] %s got=%d want=%d %s\n",
           label, (int)got, (int)want, ok ? (char*)"PASS" : (char*)"FAIL");
    if (!ok) g_budget_window_fail_count++;
}

extern "C" void run_budget_window_selftest(void) {
    // --- Case 1: basic within-window charging and exhaustion ---
    {
        BudgetWindowTracker<3, 10> t;
        check("case1 avail@0 before any charge", t.budget_available(0), true);
        t.charge(0);
        check("case1 avail@1 after 1 charge", t.budget_available(1), true);
        t.charge(1);
        check("case1 avail@2 after 2 charges", t.budget_available(2), true);
        t.charge(2);
        check("case1 avail@3 after 3 charges (== budget)", t.budget_available(3), false);
        t.charge(3);   // still same window, still over budget -- diagnostic keeps counting
        check_u64("case1 ls_charged_ticks after 4 charges", t.ls_charged_ticks(), 4);
        check_u64("case1 budget_exhaustions (exactly one transition)", t.budget_exhaustions(), 1);
    }

    // --- Case 2: window rollover resets availability, regardless of how
    // many windows were skipped in between ---
    {
        BudgetWindowTracker<3, 10> t;
        t.charge(0); t.charge(1); t.charge(2);   // exhaust window 0
        check("case2 avail@9 still exhausted (same window)", t.budget_available(9), false);
        check("case2 avail@10 next window resets", t.budget_available(10), true);
        t.charge(10);
        check("case2 avail@11 (1 charge into window 1)", t.budget_available(11), true);
        // skip straight to window 5 (ticks 50-59) without ever visiting 2-4
        check("case2 avail@55 after skipping several windows", t.budget_available(55), true);
        check_u64("case2 budget_exhaustions (only window 0 ever hit the cap)", t.budget_exhaustions(), 1);
    }

    // --- Case 3: uint32 wraparound. The exact window INDEX magnitude is
    // irrelevant (and does jump discontinuously across the wrap) -- only
    // "did the index change since last observed" matters, which stays
    // correct across the wrap. ---
    {
        BudgetWindowTracker<3, 10> t;
        uint32 near_max = 0xFFFFFFF8u;   // window index (near_max/10) is some huge number
        check("case3 avail near uint32 max, fresh", t.budget_available(near_max), true);
        t.charge(near_max); t.charge(near_max); t.charge(near_max);
        check("case3 exhausted just before wrap", t.budget_available(near_max), false);
        // now past the wrap: a small `now` value, different window index
        check("case3 avail@2 after wrapping past uint32 max", t.budget_available(2), true);
        check_u64("case3 budget_exhaustions across the wrap", t.budget_exhaustions(), 1);
    }

    printf((char*)"[budget-window-test] fail_count=%d\n", g_budget_window_fail_count);
}
