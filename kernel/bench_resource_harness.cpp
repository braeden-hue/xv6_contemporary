// EP1-RESOURCE-01 harness: correctness -> mutation-oracle check -> (codegen
// checked externally via objdump, not at runtime) -> timed benchmarks, in
// that order, per docs/episode1_next_experiment_plan.md and the
// 2026-09-13 clarifications/review. Runs after kinit() (needs a working
// kalloc).
//
// 2026-09-13 review fixes applied here:
//   - success-path caller cleanup now frees out2 THEN out1 (was out1,out2)
//     -- matches the plan's "released in reverse acquisition order" for
//     every path, not just the early-fail path.
//   - correctness checks now assert the ledger's recorded event ORDER
//     (ledger_check_*), not just "no leak / no duplicate" -- a release-order
//     bug could previously pass undetected.
//   - RES_ORDER selects which of the six ABC/BCA/CAB rotations this boot's
//     timed benchmarks use for the within-case A/B/C sequence; flipped
//     between three separate boots (see docs/bench/ep1_resource/run_order*.sh).
extern "C" {
#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

// Condition entry points (bench_resource_a.c / _b.cpp / _c.cpp).
int a_real_success(void**, void**);
int a_real_early_fail(void**, void**);
int a_fake_success(void**, void**);
int a_fake_early_fail(void**, void**);
int a_test(void**, void**, int);

int b_real_success(void**, void**);
int b_real_early_fail(void**, void**);
int b_fake_success(void**, void**);
int b_fake_early_fail(void**, void**);
int b_test(void**, void**, int);

int c_real_success(void**, void**);
int c_real_early_fail(void**, void**);
int c_fake_success(void**, void**);
int c_fake_early_fail(void**, void**);
int c_test(void**, void**, int);
int c_test_move_semantics(void);

// Ledger (bench_resource_ledger.c).
void ledger_reset(void);
void ledger_inject_fail_at(int);
int  ledger_leaked_count(void);
int  ledger_duplicate_or_unowned_detected(void);
void ledger_reclaim_all(void);
int  ledger_check_two_resource_reverse_release(void);
int  ledger_check_one_alloc_one_free(void);
int  ledger_check_no_events(void);
void mutation_omit_free(void);
void mutation_duplicate_free(void);
void* test_alloc_page(void);
void  test_free_page(void*);

// Fake allocator (bench_resource_fakealloc.c).
void  fake_reset(void);
void* fake_alloc(void);
void  fake_free(void*);
}

#define BENCH_KEEP2(p) asm volatile("" : : "r"(p))

// --- 1. Correctness: run the four scenarios against A/B/C's *_test entry
// points (real-kalloc backend, ledger-tracked). A scenario is checked
// against: the expected return code, the null/non-null contract on
// *out1/*out2, the ledger's recorded ALLOC/FREE event ORDER (not just "no
// leak"), and -- after the harness (acting as "the caller") frees whatever
// came back, out2 then out1 -- that no duplicate/unowned free was ever
// flagged. ---
struct ResourceCondition {
    const char* label;
    int (*call)(void**, void**, int);
};

static int g_correctness_fail_count = 0;

static void check(const char* cond_label, const char* case_label,
                   int got_ret, int want_ret,
                   void* out1, void* out2, int want_nonnull,
                   int order_ok) {
    int ok = 1;
    if (got_ret != want_ret) ok = 0;
    int have_nonnull = (out1 != 0) && (out2 != 0);
    int have_bothnull = (out1 == 0) && (out2 == 0);
    if (want_nonnull) {
        if (!have_nonnull) ok = 0;
    } else {
        if (!have_bothnull) ok = 0;
    }
    if (ledger_duplicate_or_unowned_detected()) ok = 0;
    if (out1 == 0 && out2 == 0 && ledger_leaked_count() != 0) ok = 0; // caller has nothing to free but ledger still holds pages
    if (!order_ok) ok = 0;

    printf((char*)"[resource-correctness] %s case=%s ret=%d(want %d) out1=%p out2=%p order_ok=%d %s\n",
           cond_label, case_label, got_ret, want_ret, out1, out2, order_ok, ok ? (char*)"PASS" : (char*)"FAIL");
    if (!ok) g_correctness_fail_count++;
}

static void run_one_condition_correctness(const ResourceCondition& c) {
    void* out1;
    void* out2;

    // Case: first allocation fails -- expect zero ledger events at all.
    ledger_reset();
    ledger_inject_fail_at(1);
    int r = c.call(&out1, &out2, 0);
    check(c.label, "alloc1_fail", r, 1, out1, out2, 0, ledger_check_no_events());

    // Case: second allocation fails -- expect ALLOC(p), FREE(p): the first
    // resource is released once the second acquisition fails.
    ledger_reset();
    ledger_inject_fail_at(2);
    r = c.call(&out1, &out2, 0);
    check(c.label, "alloc2_fail", r, 2, out1, out2, 0, ledger_check_one_alloc_one_free());

    // Case: explicit early failure after both acquisitions succeed --
    // expect ALLOC(p_a), ALLOC(p_b), FREE(p_b), FREE(p_a): released
    // in reverse acquisition order, entirely inside the entry point.
    ledger_reset();
    r = c.call(&out1, &out2, 1);
    check(c.label, "early_fail", r, 3, out1, out2, 0, ledger_check_two_resource_reverse_release());

    // Case: success + handoff -- the harness is "the caller" here, so it
    // frees both exactly once afterward, in REVERSE acquisition order
    // (out2 then out1) to match the plan's release-order requirement on
    // every path, not just early-fail. Same structural check as early_fail:
    // ALLOC(p_a), ALLOC(p_b), FREE(p_b), FREE(p_a).
    ledger_reset();
    r = c.call(&out1, &out2, 0);
    int had_both = (out1 != 0) && (out2 != 0);
    if (had_both) {
        test_free_page(out2);
        test_free_page(out1);
    }
    check(c.label, "success", r, 0, out1, out2, 1, ledger_check_two_resource_reverse_release());
}

static void run_resource_correctness_tests() {
    ResourceCondition conditions[3] = {
        {"A(C)  ", a_test},
        {"B(cpp)", b_test},
        {"C(RAII)", c_test},
    };
    for (int i = 0; i < 3; i++) {
        run_one_condition_correctness(conditions[i]);
    }

    // C-only: move construction + destruction of the moved-from owner.
    ledger_reset();
    int mv = c_test_move_semantics();
    int mv_ok = (mv == 1) && (ledger_leaked_count() == 0) && !ledger_duplicate_or_unowned_detected();
    printf((char*)"[resource-correctness] C(RAII) case=move_semantics observed_ok=%d leaked=%d dup_or_unowned=%d %s\n",
           mv, ledger_leaked_count(), ledger_duplicate_or_unowned_detected(),
           mv_ok ? (char*)"PASS" : (char*)"FAIL");
    if (!mv_ok) g_correctness_fail_count++;
}

// --- 2. Mutation-test the oracle itself: confirm the ledger actually
// catches an omitted free and a duplicate free, using isolated
// manual-cleanup snippets that are NOT any of A/B/C (see
// bench_resource_ledger.c's own comment -- these are injected bugs to test
// the checker, not a claim about A/B/C). ---
static void run_resource_mutation_tests() {
    ledger_reset();
    mutation_omit_free();
    int leaked = ledger_leaked_count();
    int detected_leak = (leaked > 0);
    printf((char*)"[resource-mutation] case=omit_free leaked_count=%d %s\n",
           leaked, detected_leak ? (char*)"DETECTED(expected)" : (char*)"MISSED(bug in oracle)");
    ledger_reclaim_all(); // return the intentionally-leaked page before continuing boot

    ledger_reset();
    mutation_duplicate_free();
    int dup = ledger_duplicate_or_unowned_detected();
    printf((char*)"[resource-mutation] case=duplicate_free dup_or_unowned_detected=%d %s\n",
           dup, dup ? (char*)"DETECTED(expected)" : (char*)"MISSED(bug in oracle)");
    ledger_reclaim_all();

    // Light sanity check for the fake backend (not the full mutation
    // contract -- see docs/episode1_next_experiment_plan.md scoping notes
    // in DECISIONS.md: correctness/mutation testing is scoped to the real
    // kalloc backend, since that's the operation whose correctness matters
    // operationally; the fake backend exists only to isolate timed cost).
    fake_reset();
    void* f1 = fake_alloc();
    void* f2 = fake_alloc();
    int fake_ok = (f1 != 0) && (f2 != 0) && (f1 != f2);
    fake_free(f1);
    fake_free(f2);
    void* f3 = fake_alloc();
    fake_ok = fake_ok && (f3 != 0);
    fake_free(f3);
    printf((char*)"[resource-mutation] case=fake_backend_sanity %s\n", fake_ok ? (char*)"PASS" : (char*)"FAIL");
}

// --- 3. Timed benchmarks. No ledger, no injection, no printf inside the
// timed loop itself -- BENCH_KEEP2 blocks dead-code elimination the same
// way kernel/bench.cpp's BENCH_KEEP does. Reports raw total, repetitions,
// and the EXACT mean as a 3-decimal fixed-point value (this kernel's
// printf has no %f) -- never just a truncated per-call integer.
//
// Cleanup order fixed to o2-then-o1 (was o1,o2) to match the plan's
// reverse-acquisition-order release requirement on the success path, same
// as the correctness harness above -- this changes what runs inside the
// timed window, so these numbers must be treated as a re-measurement, not
// a continuation of the pre-fix ones. ---
#define RES_BENCH_REPS 200

typedef int (*res_call_fn)(void**, void**);
typedef void (*res_free_fn)(void*);

static uint64 measure_resource(res_call_fn fn, res_free_fn free_fn, int reps) {
    uint64 before = r_instret();
    for (int i = 0; i < reps; i++) {
        void* o1;
        void* o2;
        int r = fn(&o1, &o2);
        BENCH_KEEP2(r);
        if (o2) { BENCH_KEEP2(o2); free_fn(o2); } // reverse acquisition order: o2 first
        if (o1) { BENCH_KEEP2(o1); free_fn(o1); }
    }
    uint64 after = r_instret();
    return after - before;
}

static void print_exact_mean(const char* label, uint64 total, int reps) {
    uint64 scaled = total * 1000 / (uint64)reps; // 3-decimal fixed point
    uint64 whole = scaled / 1000;
    uint64 frac = scaled % 1000;
    int f0 = (int)(frac / 100);
    int f1 = (int)((frac / 10) % 10);
    int f2 = (int)(frac % 10);
    printf((char*)"[resource-bench] %s total=%d reps=%d mean=%d.%d%d%d\n",
           label, (int)total, reps, (int)whole, f0, f1, f2);
}

// RES_ORDER selects the A/B/C rotation for THIS boot: 0=ABC, 1=BCA, 2=CAB.
// Flip and rebuild between three separate boots (see
// docs/bench/ep1_resource/README_order_runs.txt for the exact commands used).
#ifndef RES_ORDER
#define RES_ORDER 0
#endif

struct ResCase {
    const char* label;
    res_call_fn fn;
    res_free_fn free_fn; // frees a returned pointer on the success path
    int is_fake;          // fake backend's 4 fixed slots need a reset between cases
};

static void run_resource_benchmarks() {
    // Three conditions' cases for one backend+path combination, listed
    // A,B,C -- reordered per RES_ORDER below before running.
    ResCase real_success[3] = {
        {"A real success   ", a_real_success, kfree, 0},
        {"B real success   ", b_real_success, kfree, 0},
        {"C real success   ", c_real_success, kfree, 0},
    };
    ResCase real_fail[3] = {
        {"A real early_fail", a_real_early_fail, kfree, 0},
        {"B real early_fail", b_real_early_fail, kfree, 0},
        {"C real early_fail", c_real_early_fail, kfree, 0},
    };
    ResCase fake_success[3] = {
        {"A fake success   ", a_fake_success, fake_free, 1},
        {"B fake success   ", b_fake_success, fake_free, 1},
        {"C fake success   ", c_fake_success, fake_free, 1},
    };
    ResCase fake_fail[3] = {
        {"A fake early_fail", a_fake_early_fail, fake_free, 1},
        {"B fake early_fail", b_fake_early_fail, fake_free, 1},
        {"C fake early_fail", c_fake_early_fail, fake_free, 1},
    };
    ResCase* groups[4] = {real_success, real_fail, fake_success, fake_fail};

    static const int rot[3][3] = {
        {0, 1, 2}, // ABC
        {1, 2, 0}, // BCA
        {2, 0, 1}, // CAB
    };
    const int* order = rot[RES_ORDER];

    printf((char*)"[resource-bench] RES_ORDER=%d (0=ABC 1=BCA 2=CAB)\n", RES_ORDER);

    for (int g = 0; g < 4; g++) {
        for (int k = 0; k < 3; k++) {
            ResCase& rc = groups[g][order[k]];
            if (rc.is_fake) fake_reset();
            uint64 total = measure_resource(rc.fn, rc.free_fn, RES_BENCH_REPS);
            print_exact_mean(rc.label, total, RES_BENCH_REPS);
        }
    }
}

extern "C" void run_resource_experiment(void) {
    printf((char*)"\n[resource] EP1-RESOURCE-01 -- correctness first, then mutation check, then timed benchmarks\n");
    run_resource_correctness_tests();
    printf((char*)"[resource] correctness fail_count=%d\n", g_correctness_fail_count);
    run_resource_mutation_tests();
    run_resource_benchmarks();
}
