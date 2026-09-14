#pragma once
// SCHED-EP2-BUDGET-01 §10/§11 (design/ep2_selectonly_budget/spec.md,
// 2026-09-14 2nd review): pure, deterministic, unit-testable core for the
// high-resolution execution-volume observation pilot. Takes explicit
// r_time()-unit values as parameters -- never reads real hardware state
// itself -- so a boot-time self-test can exercise window-boundary
// splitting and priority-change splitting with synthetic inputs, the same
// discipline as kernel/sched_priority.hpp's BudgetWindowTracker.
//
// What this measures (per spec S10.2, corrected from the first draft):
// the scheduler-observed virtual elapsed time of one dispatch span,
// INCLUDING the boundary swtch() cost -- not "pure CPU service time". A
// span can be split into at most two priority-homogeneous pieces (at most
// one setpriority() call per span, matching this project's actual
// workload), and each piece is then split proportionally across whichever
// fixed-width windows it overlaps -- never attributed wholesale to the
// window it started in.
#include "types.h"

#define SPAN_MAX_WINDOWS 32

class SpanWindowAccumulator {
    uint64 window_index_[SPAN_MAX_WINDOWS];
    uint64 window_ls_time_[SPAN_MAX_WINDOWS];
    int window_count_ = 0;
    uint64 total_accounted_ = 0;   // diagnostic: sum of every piece, any priority --
                                    // must equal the sum of all add_span() durations

    int find_or_create(uint64 idx) {
        for (int i = 0; i < window_count_; i++) {
            if (window_index_[i] == idx) return i;
        }
        if (window_count_ < SPAN_MAX_WINDOWS) {
            window_index_[window_count_] = idx;
            window_ls_time_[window_count_] = 0;
            return window_count_++;
        }
        return -1;   // capacity exceeded -- documented limitation, not expected
                      // to trigger for a single short pilot run (~dozen windows)
    }

    int overflow_count_ = 0;   // find_or_create() capacity misses -- sticky, never cleared

public:
    // Adds one ALREADY priority-homogeneous piece [seg_start,
    // seg_start+duration), proportionally distributed across the
    // window_time_ticks-wide windows it overlaps. Public (2026-09-14 3rd
    // review): the corrected real wiring (kernel/span_observe.cpp) now
    // closes and classifies segments itself, event-driven, at each actual
    // setpriority() transition -- it no longer needs add_span()'s
    // after-the-fact single-change-point inference, which turned out to
    // wrongly assume a same-value setpriority() call implies a real
    // priority flip (2026-09-14 review point 2).
    void add_segment(uint64 seg_start, uint64 duration, bool is_ls, uint64 window_time_ticks) {
        if (duration == 0 || window_time_ticks == 0) return;
        uint64 seg_end = seg_start + duration;
        uint64 pos = seg_start;
        while (pos < seg_end) {
            uint64 idx = pos / window_time_ticks;
            uint64 window_end = (idx + 1) * window_time_ticks;
            uint64 piece_end = (window_end < seg_end) ? window_end : seg_end;
            uint64 piece = piece_end - pos;
            total_accounted_ += piece;
            if (is_ls) {
                int slot = find_or_create(idx);
                if (slot >= 0) {
                    window_ls_time_[slot] += piece;
                } else {
                    overflow_count_++;   // dropped -- caller must treat the whole observation as invalid
                }
            }
            pos = piece_end;
        }
    }
    // Splits a raw dispatch span [t_enter, t_exit) at an optional
    // priority-change point, then routes each resulting piece through
    // add_segment(). change_present=false (or change_time outside the
    // span) means the whole span is one piece under priority_before.
    void add_span(uint64 t_enter, uint64 t_exit,
                  bool priority_before_is_ls,
                  bool change_present, uint64 change_time, bool priority_after_is_ls,
                  uint64 window_time_ticks) {
        if (t_exit <= t_enter) return;
        if (!change_present || change_time <= t_enter || change_time >= t_exit) {
            add_segment(t_enter, t_exit - t_enter, priority_before_is_ls, window_time_ticks);
            return;
        }
        add_segment(t_enter, change_time - t_enter, priority_before_is_ls, window_time_ticks);
        add_segment(change_time, t_exit - change_time, priority_after_is_ls, window_time_ticks);
    }

    int window_count() const { return window_count_; }
    uint64 window_index(int i) const { return (i >= 0 && i < window_count_) ? window_index_[i] : 0; }
    uint64 window_ls_time(int i) const { return (i >= 0 && i < window_count_) ? window_ls_time_[i] : 0; }
    uint64 total_accounted() const { return total_accounted_; }
    // 2026-09-14 3rd review point 3: overflow must be detectable, not
    // silently dropped -- the caller (span_observe.cpp) checks this and
    // marks the whole observation run invalid rather than reporting a
    // report that silently missed some windows.
    int overflow_count() const { return overflow_count_; }
};
