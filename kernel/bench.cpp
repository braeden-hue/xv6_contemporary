// Dispatch-cost benchmarking infra (static template vs function-pointer RR).
// Kept as its own file so both the instret calibration and the later
// pick_next() comparison can reuse it without repeatedly patching main.c.
//
// NOTE: these functions run from main() *before* trapinit()/trapinithart().
// start()'s timerinit() already unmasks sie.STIE/sie.SEIE and arms the first
// timer interrupt (w_stimecmp) in M-mode, but stvec isn't installed until
// trapinithart(). Do NOT call intr_on() here: sstatus.SIE is already 0 at
// this point (nothing has enabled it yet), and turning it on early lets the
// already-armed timer interrupt fire into an uninitialized stvec -- the CPU
// traps to garbage and hangs silently (no panic, since it never reaches the
// real trap handler). Found by bisection: main() hung between "after
// run_dispatch_bench" and "after kinit" printfs whenever this file called
// intr_off()/intr_on(); removing both calls fixed it outright.

extern "C" {
#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"
}

// Calibration: confirm minstret increments exactly 1 per retired instruction
// in this exact QEMU build, before trusting it for the real comparison.
// The loop body is exactly 2 instructions (addi, bnez); the setup mv is 1.
// Expected instret delta for n iterations: 1 + 2*n.
static uint64 calibrate_once(uint64 n) {
    uint64 before = r_instret();
    asm volatile(
        "mv t0, %0\n"
        "1:\n"
        "addi t0, t0, -1\n"
        "bnez t0, 1b\n"
        : : "r"(n) : "t0", "memory"
    );
    uint64 after = r_instret();
    return after - before;
}

extern "C" void run_calibration(void) {
    // Diagnostic: call with the SAME n repeatedly, to see whether only the
    // very first invocation (regardless of n) is the anomaly.
    uint64 repeat[20];
    for (int i = 0; i < 20; i++) {
        repeat[i] = calibrate_once(1000);
    }

    uint64 ns[3] = {1000, 2000, 4000};
    uint64 deltas[3];
    for (int i = 0; i < 3; i++) {
        deltas[i] = calibrate_once(ns[i]);
    }

    for (int i = 0; i < 20; i++) {
        printf((char*)"[repeat n=1000] call=%d instret_delta=%d\n", i, (int)repeat[i]);
    }

    for (int i = 0; i < 3; i++) {
        uint64 expected = 1 + 2 * ns[i];
        printf((char*)"[calib] n=%d instret_delta=%d expected=%d diff=%d\n",
               (int)ns[i], (int)deltas[i], (int)expected,
               (int)((long)deltas[i] - (long)expected));
    }
    // slope check: isolates fixed overhead from the true per-instruction rate
    printf((char*)"[calib] slope(2000-1000)=%d expected_slope=2000\n",
           (int)(deltas[1] - deltas[0]));
    printf((char*)"[calib] slope(4000-2000)=%d expected_slope=4000\n",
           (int)(deltas[2] - deltas[1]));
}

// --- Main experiment: static (template) dispatch vs function-pointer dispatch ---
// Baseline, as confirmed with the user: identical RR body and identical initial
// state for both mechanisms; the measured window is pick_next() calls only
// (not the surrounding dispatch() loop); a trivial O(1) body is measured first
// to isolate pure call-path cost from policy-body cost.

#include "sched_rr.hpp"   // struct RR -- byte-for-byte the production policy

#define BENCH_NPROC NPROC
#define BENCH_REPS  200

static struct proc bench_procs[BENCH_NPROC];

// Deterministic worst case: only the last slot is RUNNABLE, so RR's scan
// never short-circuits early -- every single call walks all 64 slots.
static void bench_reset_procs() {
    for (int i = 0; i < BENCH_NPROC; i++)
        bench_procs[i].state = UNUSED;
    bench_procs[BENCH_NPROC - 1].state = RUNNABLE;
}

// O(1) body, to isolate call-path cost from policy-body cost.
struct Trivial {
    struct proc* pick_next(struct proc* procs, int) { return &procs[0]; }
};
static struct proc* fnptr_trivial_pick_next(struct proc* procs, int) {
    return &procs[0];
}

// Real RR body, function-pointer mechanism -- logic copied verbatim from
// sched_rr.hpp's RR::pick_next so the two mechanisms run identical code.
static int fnptr_rr_last = -1;
static struct proc* fnptr_rr_pick_next(struct proc* procs, int n) {
    for (int i = 1; i <= n; i++) {
        int idx = (fnptr_rr_last + i) % n;
        struct proc* p = &procs[idx];
        if (p->state == RUNNABLE) {
            fnptr_rr_last = idx;
            return p;
        }
    }
    return nullptr;
}

typedef struct proc* (*pick_next_fn)(struct proc*, int);

// volatile: a single-assignment, whole-TU-visible function pointer is exactly
// the pattern -O devirtualizes back into a direct call, which would silently
// defeat the point of this comparison. volatile forces a real reload + jalr.
static pick_next_fn volatile g_trivial_fn = fnptr_trivial_pick_next;
static pick_next_fn volatile g_rr_fn = fnptr_rr_pick_next;

// "Do not optimize away": marks p as used (blocks dead-code elim) without a
// "memory" clobber. A full memory clobber here would force the *inlined*
// static loop to spill/reload policy.last to the stack every iteration (an
// artifact of the barrier itself, not of static dispatch), while the fnptr
// loop pays that reload anyway through the opaque call boundary -- so a
// per-iteration memory clobber would unfairly penalize only the static side.
// RR::pick_next mutates `last` each call (not loop-invariant), so neither
// loop can be folded or hoisted even without it.
#define BENCH_KEEP(p) asm volatile("" : : "r"(p))

static uint64 measure_static_trivial(int reps) {
    Trivial policy;
    uint64 before = r_instret();
    for (int i = 0; i < reps; i++) {
        struct proc* p = policy.pick_next(bench_procs, BENCH_NPROC);
        BENCH_KEEP(p);
    }
    uint64 after = r_instret();
    return after - before;
}

static uint64 measure_fnptr_trivial(int reps) {
    uint64 before = r_instret();
    for (int i = 0; i < reps; i++) {
        struct proc* p = g_trivial_fn(bench_procs, BENCH_NPROC);
        BENCH_KEEP(p);
    }
    uint64 after = r_instret();
    return after - before;
}

static uint64 measure_static_rr(int reps) {
    RR policy;
    uint64 before = r_instret();
    for (int i = 0; i < reps; i++) {
        struct proc* p = policy.pick_next(bench_procs, BENCH_NPROC);
        BENCH_KEEP(p);
    }
    uint64 after = r_instret();
    return after - before;
}

static uint64 measure_fnptr_rr(int reps) {
    uint64 before = r_instret();
    for (int i = 0; i < reps; i++) {
        struct proc* p = g_rr_fn(bench_procs, BENCH_NPROC);
        BENCH_KEEP(p);
    }
    uint64 after = r_instret();
    return after - before;
}

// Isolation experiment: same static/inlined RR body, but `n` is read through
// a volatile global instead of passed as the BENCH_NPROC compile-time
// constant. This blocks GCC from constant-folding the divisor in `(last+i)%n`,
// which is what made the plain static-RR loop emit a 6-instruction software
// signed-modulo-by-constant sequence instead of the single hardware `remw`
// the compiler uses when n's value isn't known at compile time (as in the
// fnptr build, and as in any real un-inlined policy). Separates "cost of
// static dispatch" from "cost of inlining letting the compiler see n=64".
static volatile int g_opaque_n = BENCH_NPROC;

static uint64 measure_static_rr_opaque_n(int reps) {
    RR policy;
    uint64 before = r_instret();
    for (int i = 0; i < reps; i++) {
        struct proc* p = policy.pick_next(bench_procs, g_opaque_n);
        BENCH_KEEP(p);
    }
    uint64 after = r_instret();
    return after - before;
}

extern "C" void run_dispatch_bench(void) {
    bench_reset_procs();
    uint64 trivial_static = measure_static_trivial(BENCH_REPS);
    bench_reset_procs();
    uint64 trivial_fnptr = measure_fnptr_trivial(BENCH_REPS);

    bench_reset_procs();
    uint64 rr_static = measure_static_rr(BENCH_REPS);
    fnptr_rr_last = -1;
    bench_reset_procs();
    uint64 rr_fnptr = measure_fnptr_rr(BENCH_REPS);

    bench_reset_procs();
    uint64 rr_static_opaque = measure_static_rr_opaque_n(BENCH_REPS);

    printf((char*)"[dispatch] trivial static total=%d per_call=%d\n",
           (int)trivial_static, (int)(trivial_static / BENCH_REPS));
    printf((char*)"[dispatch] trivial fnptr  total=%d per_call=%d\n",
           (int)trivial_fnptr, (int)(trivial_fnptr / BENCH_REPS));
    printf((char*)"[dispatch] call-path delta (fnptr - static), trivial body, per_call=%d\n",
           (int)(((long)trivial_fnptr - (long)trivial_static) / BENCH_REPS));

    printf((char*)"[dispatch] RR static total=%d per_call=%d\n",
           (int)rr_static, (int)(rr_static / BENCH_REPS));
    printf((char*)"[dispatch] RR fnptr  total=%d per_call=%d\n",
           (int)rr_fnptr, (int)(rr_fnptr / BENCH_REPS));
    printf((char*)"[dispatch] RR total delta (fnptr - static), per_call=%d\n",
           (int)(((long)rr_fnptr - (long)rr_static) / BENCH_REPS));

    printf((char*)"[dispatch] RR static (opaque n) total=%d per_call=%d\n",
           (int)rr_static_opaque, (int)(rr_static_opaque / BENCH_REPS));
    printf((char*)"[dispatch] RR opaque-n delta vs fnptr, per_call=%d (isolates call-path cost once the constant-n codegen confound is removed)\n",
           (int)(((long)rr_fnptr - (long)rr_static_opaque) / BENCH_REPS));
}
