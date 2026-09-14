// SCHED-EP2-BUDGET-01 S11: real wiring for the high-resolution
// execution-volume observation pilot. Pure observation only -- never
// changes any scheduling decision. Single global instance, same CPUS=1-only
// caveat as kernel/sched_priority.hpp's PrioritySelectBudget (no
// synchronization on the shared state).
//
// Rewritten 2026-09-14 (3rd review) to fix two real bugs in the first
// version:
//   1. The old span_observe_dispatch_span() reconstructed AFTER THE FACT
//      from a single "last priority change" record, inferring the
//      pre-change priority as `!post_change_priority`. That inference is
//      wrong whenever setpriority() is called with the SAME value the
//      proc already had (QD's LONG tasks call setpriority(0) while
//      already Normal=0) -- the old code would still treat that as a real
//      LS->Normal flip. Replaced with an EVENT-DRIVEN model: a segment is
//      opened at dispatch-entry and explicitly closed/reopened at each
//      setpriority() call that actually changes the value, so a same-value
//      call is correctly a no-op, and a same-span 0->1->0 sequence
//      produces the right number of segments instead of being flattened
//      to "the last change only".
//   2. window_time_ticks (the r_time-unit window width) was presented as
//      if it were an exact match for the existing tick-based
//      PrioritySelectBudget window. It is not: `ticks` and `r_time()` do
//      not share a guaranteed common origin, and clockintr()'s
//      `w_stimecmp(r_time() + 1000000)` sets the NEXT deadline, not a
//      guarantee that consecutive ticks are always exactly 1,000,000
//      r_time units apart (interrupt handling latency can widen a given
//      tick). This file now also EMPIRICALLY records (tick, r_time) pairs
//      from clockintr() itself and reports the measured ratio for this
//      specific run, instead of asserting the nominal constant as fact.
//      The report is labelled a "time-unit observation window", not
//      claimed equivalent to the tick-based window.
extern "C" {
#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
}

#include "span_observe_config.h"
#include "span_window_accumulator.hpp"

// Nominal window width in r_time units, for PrioritySelectBudget's
// WindowTicks=10 -- NOT asserted as exact (see file header point 2).
// Reported alongside the empirically measured tick<->r_time ratio from
// this same run so a reader can judge the actual correspondence
// themselves rather than trust this constant blindly.
#define EP2_SPAN_TICKS_PER_WINDOW 10
#define EP2_SPAN_NOMINAL_RTIME_PER_TICK 1000000ULL   // kernel/trap.c clockintr()'s re-arm interval (nominal, not measured)
#define EP2_SPAN_WINDOW_TIME_TICKS (EP2_SPAN_TICKS_PER_WINDOW * EP2_SPAN_NOMINAL_RTIME_PER_TICK)

#if EP2_SPAN_OBSERVE_ENABLED

static SpanWindowAccumulator g_span_acc;

// Per-proc-slot OPEN SEGMENT state, indexed by pointer arithmetic from the
// global `proc` array (kernel/proc.h) -- same isolation pattern as
// g_ls_runnable_count: no proc.h field added.
static uint64 g_seg_start[NPROC];
static int g_seg_priority[NPROC];
static int g_seg_open[NPROC];

// Empirical tick<->r_time calibration, recorded from clockintr() itself
// (real tick boundaries, not assumed ones). First and latest samples only
// -- enough for an average-ratio-over-this-run figure.
static uint64 g_calib_first_ticks = 0, g_calib_first_rtime = 0;
static uint64 g_calib_latest_ticks = 0, g_calib_latest_rtime = 0;
static int g_calib_have_first = 0;

static int slot_of(struct proc* p) {
    int idx = (int)(p - proc);
    if (idx < 0 || idx >= NPROC) return -1;
    return idx;
}

extern "C" void span_observe_clockintr_tick(void) {
    uint64 now_ticks = __atomic_load_n(&ticks, __ATOMIC_RELAXED);
    uint64 now_rtime = r_time();
    if (!g_calib_have_first) {
        g_calib_first_ticks = now_ticks;
        g_calib_first_rtime = now_rtime;
        g_calib_have_first = 1;
    }
    g_calib_latest_ticks = now_ticks;
    g_calib_latest_rtime = now_rtime;
}

// Opens a fresh segment for `p` at dispatch entry -- priority read
// DIRECTLY (p->priority at this exact instant), never inferred.
extern "C" void span_observe_dispatch_enter(struct proc* p) {
    int idx = slot_of(p);
    if (idx < 0) return;
    g_seg_start[idx] = r_time();
    g_seg_priority[idx] = p->priority;
    g_seg_open[idx] = 1;
}

// Closes the final open segment for `p` at dispatch exit (t_exit already
// captured by the caller, right after swtch() returns).
extern "C" void span_observe_dispatch_exit(struct proc* p, uint64 t_exit) {
    int idx = slot_of(p);
    if (idx < 0 || !g_seg_open[idx]) return;
    if (t_exit > g_seg_start[idx]) {
        g_span_acc.add_segment(g_seg_start[idx], t_exit - g_seg_start[idx],
                                g_seg_priority[idx] == 1, EP2_SPAN_WINDOW_TIME_TICKS);
    }
    g_seg_open[idx] = 0;
}

// Called from sys_setpriority() AFTER myproc()->priority has already been
// updated to the new value. Closes the segment that was open under the
// OLD priority up to now, then reopens a new segment under the NEW
// priority -- but ONLY if the value actually changed (fixes bug 1: a
// same-value call, e.g. a LONG task's Normal->Normal setpriority(0), must
// be a no-op here, not misread as a real transition).
extern "C" void span_observe_priority_change(struct proc* p, int old_priority) {
    int idx = slot_of(p);
    if (idx < 0 || !g_seg_open[idx]) return;
    if (p->priority == old_priority) return;   // no real transition -- bug 1 fix

    uint64 now = r_time();
    if (now > g_seg_start[idx]) {
        g_span_acc.add_segment(g_seg_start[idx], now - g_seg_start[idx],
                                g_seg_priority[idx] == 1, EP2_SPAN_WINDOW_TIME_TICKS);
    }
    g_seg_start[idx] = now;
    g_seg_priority[idx] = p->priority;
    // g_seg_open[idx] stays 1 -- still mid-dispatch, just under a new priority
}

extern "C" void span_observe_dump(void) {
    uint64 calib_ticks_elapsed = (g_calib_have_first && g_calib_latest_ticks > g_calib_first_ticks)
        ? (g_calib_latest_ticks - g_calib_first_ticks) : 0;
    uint64 calib_rtime_elapsed = (g_calib_have_first && g_calib_latest_rtime > g_calib_first_rtime)
        ? (g_calib_latest_rtime - g_calib_first_rtime) : 0;

    printf((char*)"[span-observe] UNIT=time-unit observation window (NOT asserted identical to the "
           "tick-based PrioritySelectBudget window -- see file header) nominal_window_rtime_units=%lld "
           "(=%d ticks x %lld nominal_rtime_per_tick)\n",
           (long long)EP2_SPAN_WINDOW_TIME_TICKS, EP2_SPAN_TICKS_PER_WINDOW,
           (long long)EP2_SPAN_NOMINAL_RTIME_PER_TICK);
    printf((char*)"[span-observe] empirical_calibration ticks_elapsed=%lld rtime_elapsed=%lld "
           "(measured from real clockintr() tick boundaries THIS run, not assumed)\n",
           (long long)calib_ticks_elapsed, (long long)calib_rtime_elapsed);

    int overflow = g_span_acc.overflow_count();
    printf((char*)"[span-observe] total_accounted=%lld window_count=%d overflow_count=%d %s\n",
           (long long)g_span_acc.total_accounted(), g_span_acc.window_count(), overflow,
           overflow ? (char*)"INVALID(overflow -- window table incomplete, do not trust)" : (char*)"valid");
    for (int i = 0; i < g_span_acc.window_count(); i++) {
        printf((char*)"[span-observe] window_index=%lld ls_time=%lld\n",
               (long long)g_span_acc.window_index(i), (long long)g_span_acc.window_ls_time(i));
    }
}

#else  // !EP2_SPAN_OBSERVE_ENABLED -- OFF build: hooks exist but are never
       // called (call sites themselves are compiled out in scheduler.cpp
       // /sysproc.c/trap.c), these bodies exist only so a stray extern
       // declaration elsewhere still links.

extern "C" void span_observe_clockintr_tick(void) {}
extern "C" void span_observe_dispatch_enter(struct proc*) {}
extern "C" void span_observe_dispatch_exit(struct proc*, uint64) {}
extern "C" void span_observe_priority_change(struct proc*, int) {}
extern "C" void span_observe_dump(void) {
    printf((char*)"[span-observe] DISABLED (EP2_SPAN_OBSERVE_ENABLED=0 build)\n");
}

#endif
