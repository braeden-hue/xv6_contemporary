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

// --- SCHED-VANILLA-DISPATCH-01: vanilla xv6 scan vs current RR::pick_next() ---
// Design doc: agent-management/projects/xv6_os_project/design/
// vanilla_cpp_rr_dispatch_bench/spec.md
//
// vanilla_style_pick_next() is the RUNNABLE-scan from this repo's own
// pre-Phase-1 scheduler() (git commit cb4e0c1, kernel/proc.c:441-446),
// quoted verbatim below for reference:
//
//   for(p = proc; p < &proc[NPROC]; p++) {
//     acquire(&p->lock);
//     if(p->state == RUNNABLE) {
//       p->state = RUNNING;
//       c->proc = p;
//       swtch(&c->context, &p->context);
//       c->proc = 0;
//       found = 1;
//     }
//     release(&p->lock);
//   }
//
// Adapted (per spec.md SS4) into a pick_next()-shaped function so it can be
// measured the same way Phase 1 measured RR::pick_next():
//   - acquire()/release()/swtch() removed -- Phase 1's own pick_next()
//     measurements exclude locking and the context switch too, so this
//     keeps both sides on the same boundary (search cost only).
//   - an early "return on first match" ADDED. The original loop has no
//     such thing -- it keeps sweeping and would switch to every RUNNABLE
//     proc it meets in one pass. This is the one deliberate change from
//     verbatim; see spec.md SS3 for why a literal port isn't possible.
// Do not read this as "vanilla xv6's real dispatch cost" -- see spec.md
// SS7 (non-goals) before quoting a number out of this function's context.
static struct proc* vanilla_style_pick_next(struct proc* procs, int n) {
    for (struct proc* p = procs; p < &procs[n]; p++) {
        if (p->state == RUNNABLE) {
            return p;   // adaptation, not in the original loop
        }
    }
    return nullptr;
}

static uint64 measure_vanilla_style(int reps) {
    uint64 before = r_instret();
    for (int i = 0; i < reps; i++) {
        struct proc* p = vanilla_style_pick_next(bench_procs, BENCH_NPROC);
        BENCH_KEEP(p);
    }
    uint64 after = r_instret();
    return after - before;
}

// --- SCHED-EPISODE1-ABC-01: C direct vs C++ template vs C++ fnptr ---
// The question this isolates: "does separating the policy into a C++
// template cost more than writing the same search directly in C?" -- a
// different question from Phase 1's "static vs fnptr dispatch *within*
// C++", and from SCHED-VANILLA-DISPATCH-01's "this project's RR vs the
// original xv6 scheduler()'s unrelated algorithm". Three conditions, all
// with search order/last-update/wrap/n-as-runtime-parameter held identical
// (verified below, not assumed):
//   A. kernel/bench_rr_direct.c      -- plain C, compiled by $(CC) (gcc-14)
//   B. kernel/bench_rr_direct_cpp.cpp -- same RR policy object, __attribute__
//      ((noinline)) wrapper forces it out-of-line, compiled by $(CXX)
//   C. fnptr_rr_pick_next above       -- existing indirect-call contrast
// A and B are both reached through an ordinary out-of-line direct call
// (neither can be inlined into this TU -- A because it's a separate .c
// file with no LTO, B because of the noinline attribute) so neither side
// is unfairly given an optimization opportunity the other lacks; this is
// what makes A vs B the fair "abstraction cost" comparison, unlike
// comparing A against the *inlined* rr_static/rr_static_opaque numbers
// above would have been.
extern "C" {
struct proc* c_rr_pick_next(struct proc* procs, int n);
void c_rr_reset_last(void);
int c_rr_get_last(void);
struct proc* cpp_rr_pick_next_noinline(struct proc* procs, int n);
void cpp_rr_direct_reset_last(void);
int cpp_rr_direct_get_last(void);
}

static uint64 measure_c_direct(int reps) {
    uint64 before = r_instret();
    for (int i = 0; i < reps; i++) {
        struct proc* p = c_rr_pick_next(bench_procs, BENCH_NPROC);
        BENCH_KEEP(p);
    }
    uint64 after = r_instret();
    return after - before;
}

static uint64 measure_cpp_direct_noinline(int reps) {
    uint64 before = r_instret();
    for (int i = 0; i < reps; i++) {
        struct proc* p = cpp_rr_pick_next_noinline(bench_procs, BENCH_NPROC);
        BENCH_KEEP(p);
    }
    uint64 after = r_instret();
    return after - before;
}

// Correctness gate, run before any timing: from the SAME reset state
// (bench_reset_procs(), last=-1), confirm all four call paths -- A, B
// out-of-line, B inlined (existing RR policy object), and C (fnptr) --
// pick the same proc index and update `last` to the same value, call for
// call. If these sequences ever diverge, the instret numbers below are not
// comparing the same algorithm and must not be trusted.
static void verify_abc_equivalence(int calls) {
    bench_reset_procs();
    c_rr_reset_last();
    for (int i = 0; i < calls; i++) {
        struct proc* p = c_rr_pick_next(bench_procs, BENCH_NPROC);
        int idx = p ? (int)(p - bench_procs) : -1;
        printf((char*)"[verify] A(C-direct)     call=%d idx=%d last=%d\n", i, idx, c_rr_get_last());
    }

    bench_reset_procs();
    cpp_rr_direct_reset_last();
    for (int i = 0; i < calls; i++) {
        struct proc* p = cpp_rr_pick_next_noinline(bench_procs, BENCH_NPROC);
        int idx = p ? (int)(p - bench_procs) : -1;
        printf((char*)"[verify] B(cpp-noinline) call=%d idx=%d last=%d\n", i, idx, cpp_rr_direct_get_last());
    }

    {
        RR policy;
        bench_reset_procs();
        for (int i = 0; i < calls; i++) {
            struct proc* p = policy.pick_next(bench_procs, BENCH_NPROC);
            int idx = p ? (int)(p - bench_procs) : -1;
            printf((char*)"[verify] B(cpp-inlined)  call=%d idx=%d last=%d\n", i, idx, policy.last);
        }
    }

    bench_reset_procs();
    fnptr_rr_last = -1;
    for (int i = 0; i < calls; i++) {
        struct proc* p = g_rr_fn(bench_procs, BENCH_NPROC);
        int idx = p ? (int)(p - bench_procs) : -1;
        printf((char*)"[verify] C(fnptr)        call=%d idx=%d last=%d\n", i, idx, fnptr_rr_last);
    }
}

// --- Boundary-case equivalence check (per 2026-09-13 feedback) ---
// The single-input check above (only the last slot RUNNABLE, 5 calls) is an
// equivalence check for THAT input only, not general equivalence. This adds
// three more cases: an empty RUNNABLE set, several scattered RUNNABLE procs
// (exercises cycling past the count), and a wraparound forced by a
// mid-sequence state change (the RUNNABLE set changes between two calls,
// same as a real scheduler would see between dispatch() invocations).
typedef struct proc* (*rr_call_fn)(struct proc*, int);
typedef void (*rr_reset_fn)(void);
typedef int (*rr_get_last_fn)(void);

struct BenchCondition {
    const char* label;
    rr_reset_fn reset_last;
    rr_call_fn call;
    rr_get_last_fn get_last;
};

// B(inlined) and C(fnptr) don't already expose a reset/call/get_last triple
// with this exact signature (B-inlined is a method on a stack object in the
// earlier verify function; C's state is two loose statics) -- small
// wrappers here, not used by the timed measurements above, only by this
// boundary check.
static RR g_boundary_inlined_policy;
static void b_inlined_reset_last() { g_boundary_inlined_policy.last = -1; }
static struct proc* b_inlined_call(struct proc* procs, int n) {
    return g_boundary_inlined_policy.pick_next(procs, n);
}
static int b_inlined_get_last() { return g_boundary_inlined_policy.last; }

static void fnptr_reset_last() { fnptr_rr_last = -1; }
static struct proc* fnptr_call_wrapper(struct proc* procs, int n) { return g_rr_fn(procs, n); }
static int fnptr_get_last() { return fnptr_rr_last; }

static void bench_set_all_unused() {
    for (int i = 0; i < BENCH_NPROC; i++)
        bench_procs[i].state = UNUSED;
}

static void bench_set_runnable_indices(const int* idxs, int count) {
    bench_set_all_unused();
    for (int i = 0; i < count; i++)
        bench_procs[idxs[i]].state = RUNNABLE;
}

static void run_boundary_script(const BenchCondition& c) {
    // Case 1: empty RUNNABLE set.
    bench_set_all_unused();
    c.reset_last();
    struct proc* p0 = c.call(bench_procs, BENCH_NPROC);
    printf((char*)"[verify-boundary] %s case=empty     idx=%d last=%d (expect idx=-1 last=-1)\n",
           c.label, p0 ? (int)(p0 - bench_procs) : -1, c.get_last());

    // Case 2: several scattered RUNNABLE procs, called past the count so the
    // circular scan must wrap and repeat.
    int scattered[3] = {5, 20, 40};
    bench_set_runnable_indices(scattered, 3);
    c.reset_last();
    for (int i = 0; i < 6; i++) {
        struct proc* p = c.call(bench_procs, BENCH_NPROC);
        printf((char*)"[verify-boundary] %s case=scattered call=%d idx=%d last=%d\n",
               c.label, i, p ? (int)(p - bench_procs) : -1, c.get_last());
    }

    // Case 3: wraparound forced by a mid-sequence state change -- select
    // idx=62 first (last becomes 62, near the top of the range), then the
    // RUNNABLE set changes to just idx=0 before the next call, forcing the
    // scan to wrap past the array end (63) to reach it.
    {
        int high[1] = {62};
        bench_set_runnable_indices(high, 1);
        c.reset_last();
        struct proc* r1 = c.call(bench_procs, BENCH_NPROC);
        int first_idx = r1 ? (int)(r1 - bench_procs) : -1;
        int first_last = c.get_last();

        int low[1] = {0};
        bench_set_runnable_indices(low, 1);   // state changes between calls
        struct proc* r2 = c.call(bench_procs, BENCH_NPROC);
        int wrapped_idx = r2 ? (int)(r2 - bench_procs) : -1;
        int wrapped_last = c.get_last();

        printf((char*)"[verify-boundary] %s case=wrap first_idx=%d first_last=%d wrapped_idx=%d wrapped_last=%d (expect 62,62,0,0)\n",
               c.label, first_idx, first_last, wrapped_idx, wrapped_last);
    }
}

static void verify_abc_boundary_cases() {
    BenchCondition conditions[4] = {
        {"A(C-direct)    ", c_rr_reset_last,        c_rr_pick_next,             c_rr_get_last},
        {"B(cpp-noinline)", cpp_rr_direct_reset_last, cpp_rr_pick_next_noinline, cpp_rr_direct_get_last},
        {"B(cpp-inlined) ", b_inlined_reset_last,    b_inlined_call,             b_inlined_get_last},
        {"C(fnptr)       ", fnptr_reset_last,        fnptr_call_wrapper,         fnptr_get_last},
    };
    for (int i = 0; i < 4; i++) {
        run_boundary_script(conditions[i]);
    }
}

extern "C" void run_dispatch_bench(void) {
    verify_abc_equivalence(5);
    verify_abc_boundary_cases();

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

    bench_reset_procs();
    uint64 vanilla_style = measure_vanilla_style(BENCH_REPS);

    c_rr_reset_last();
    bench_reset_procs();
    uint64 c_direct = measure_c_direct(BENCH_REPS);

    cpp_rr_direct_reset_last();
    bench_reset_procs();
    uint64 cpp_direct_noinline = measure_cpp_direct_noinline(BENCH_REPS);

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

    printf((char*)"[dispatch] vanilla-style scan (adapted from pre-Phase-1 scheduler(), no lock/swtch) total=%d per_call=%d\n",
           (int)vanilla_style, (int)(vanilla_style / BENCH_REPS));
    printf((char*)"[dispatch] vanilla-style vs cpp RR static delta (vanilla - cpp static), per_call=%d\n",
           (int)(((long)vanilla_style - (long)rr_static) / BENCH_REPS));
    printf((char*)"[dispatch] vanilla-style vs cpp RR static-opaque-n delta (vanilla - cpp static opaque-n), per_call=%d\n",
           (int)(((long)vanilla_style - (long)rr_static_opaque) / BENCH_REPS));

    printf((char*)"[dispatch] A: RR direct-C (out-of-line, gcc-14) total=%d per_call=%d\n",
           (int)c_direct, (int)(c_direct / BENCH_REPS));
    printf((char*)"[dispatch] B: RR cpp-template noinline (out-of-line, g++-14) total=%d per_call=%d\n",
           (int)cpp_direct_noinline, (int)(cpp_direct_noinline / BENCH_REPS));
    printf((char*)"[dispatch] C: RR fnptr (existing, out-of-line + indirect call) total=%d per_call=%d\n",
           (int)rr_fnptr, (int)(rr_fnptr / BENCH_REPS));
    printf((char*)"[dispatch] A vs B delta (direct-C - cpp-noinline), per_call=%d (both out-of-line, direct call, opaque-n, same -O -- the fair abstraction-cost comparison)\n",
           (int)(((long)c_direct - (long)cpp_direct_noinline) / BENCH_REPS));
    printf((char*)"[dispatch] B vs C delta (cpp-noinline - fnptr), per_call=%d (isolates direct call vs indirect/fnptr call, both out-of-line C++)\n",
           (int)(((long)cpp_direct_noinline - (long)rr_fnptr) / BENCH_REPS));
    printf((char*)"[dispatch] A vs B(inlined) delta (direct-C - cpp static-opaque-n), per_call=%d (NOT apples-to-apples -- B here is inlined, shown for context only)\n",
           (int)(((long)c_direct - (long)rr_static_opaque) / BENCH_REPS));
}
