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
