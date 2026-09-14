// SCHED-EP2-BUDGET-01 S11 step 2: deterministic, boot-time self-test of
// SpanWindowAccumulator using SYNTHETIC r_time-unit values -- never the
// real `time` CSR. Exercises: a span fully inside one window, a span
// crossing exactly one window boundary, a span crossing several windows,
// and a span with a mid-span priority change -- checking in every case
// that the split pieces sum back to the original span length (the
// correctness property §11 asks for explicitly).
extern "C" {
#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
}

#include "span_window_accumulator.hpp"

static int g_span_selftest_fail_count = 0;

static void check_u64(const char* label, uint64 got, uint64 want) {
    bool ok = (got == want);
    printf((char*)"[span-window-test] %s got=%d want=%d %s\n",
           label, (int)got, (int)want, ok ? (char*)"PASS" : (char*)"FAIL");
    if (!ok) g_span_selftest_fail_count++;
}

extern "C" void run_span_window_selftest(void) {
    const uint64 W = 100;   // window width, r_time units, for a readable example

    // --- Case 1: span fully inside one window, all LS ---
    {
        SpanWindowAccumulator acc;
        acc.add_span(1000, 1030, /*before_ls=*/true, /*change=*/false, 0, false, W);
        check_u64("case1 total_accounted", acc.total_accounted(), 30);
        check_u64("case1 window_count", (uint64)acc.window_count(), 1);
        check_u64("case1 window_index[0]", acc.window_index(0), 10);
        check_u64("case1 window_ls_time[0]", acc.window_ls_time(0), 30);
    }

    // --- Case 2: span crosses exactly one window boundary (1100), all LS ---
    {
        SpanWindowAccumulator acc;
        acc.add_span(1080, 1120, true, false, 0, false, W);
        check_u64("case2 total_accounted", acc.total_accounted(), 40);
        check_u64("case2 window_count", (uint64)acc.window_count(), 2);
        // window 10 gets [1080,1100) = 20, window 11 gets [1100,1120) = 20
        uint64 w10 = 0, w11 = 0;
        for (int i = 0; i < acc.window_count(); i++) {
            if (acc.window_index(i) == 10) w10 = acc.window_ls_time(i);
            if (acc.window_index(i) == 11) w11 = acc.window_ls_time(i);
        }
        check_u64("case2 window[10] piece", w10, 20);
        check_u64("case2 window[11] piece", w11, 20);
    }

    // --- Case 3: span crosses several windows (10, 11, 12), all LS ---
    {
        SpanWindowAccumulator acc;
        acc.add_span(1050, 1250, true, false, 0, false, W);
        check_u64("case3 total_accounted", acc.total_accounted(), 200);
        check_u64("case3 window_count", (uint64)acc.window_count(), 3);
        uint64 w10 = 0, w11 = 0, w12 = 0;
        for (int i = 0; i < acc.window_count(); i++) {
            if (acc.window_index(i) == 10) w10 = acc.window_ls_time(i);
            if (acc.window_index(i) == 11) w11 = acc.window_ls_time(i);
            if (acc.window_index(i) == 12) w12 = acc.window_ls_time(i);
        }
        // [1050,1100)=50 -> w10, [1100,1200)=100 -> w11, [1200,1250)=50 -> w12
        check_u64("case3 window[10] piece", w10, 50);
        check_u64("case3 window[11] piece", w11, 100);
        check_u64("case3 window[12] piece", w12, 50);
    }

    // --- Case 4: mid-span priority change (LS -> Normal at 1050), within
    // one window -- only the LS-priority piece should count toward
    // window_ls_time, but total_accounted (both priorities) still sums to
    // the full span length. ---
    {
        SpanWindowAccumulator acc;
        acc.add_span(1000, 1100, /*before_ls=*/true, /*change=*/true, 1050,
                     /*after_ls=*/false, W);
        check_u64("case4 total_accounted (both pieces, any priority)", acc.total_accounted(), 100);
        check_u64("case4 window_count", (uint64)acc.window_count(), 1);
        // only the [1000,1050) LS piece counts -- the [1050,1100) Normal
        // piece is accounted in total_accounted() but not window_ls_time()
        check_u64("case4 window[10] ls_time (LS piece only, not the Normal piece)",
                  acc.window_ls_time(0), 50);
    }

    // --- Case 5: mid-span priority change ALSO crossing a window boundary
    // -- both splits must compose correctly. Change at 1090 (LS->Normal),
    // span [1050,1150). LS piece [1050,1090) is 40, entirely in window 10.
    // Normal piece [1090,1150) is 60, split window10:[1090,1100)=10 (not
    // LS, doesn't count) + window11:[1100,1150)=50 (not LS, doesn't
    // count). window_ls_time should show only window10 += 40.
    {
        SpanWindowAccumulator acc;
        acc.add_span(1050, 1150, true, true, 1090, false, W);
        check_u64("case5 total_accounted", acc.total_accounted(), 100);
        uint64 w10 = 0, w11 = 0;
        int w11_present = 0;
        for (int i = 0; i < acc.window_count(); i++) {
            if (acc.window_index(i) == 10) w10 = acc.window_ls_time(i);
            if (acc.window_index(i) == 11) { w11 = acc.window_ls_time(i); w11_present = 1; }
        }
        check_u64("case5 window[10] ls_time (LS piece only)", w10, 40);
        // window 11 may or may not have been created (the Normal piece
        // touches it but contributes 0 to ls_time either way) -- if it
        // exists, its ls_time must be exactly 0.
        if (w11_present) {
            check_u64("case5 window[11] ls_time (Normal-only piece)", w11, 0);
        }
    }

    printf((char*)"[span-window-test] fail_count=%d\n", g_span_selftest_fail_count);
}
