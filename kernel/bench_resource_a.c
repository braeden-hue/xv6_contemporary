// EP1-RESOURCE-01, condition A: plain C, manual acquire/release, one
// cleanup path per branch, no ownership object at all. Compiled by $(CC)
// (gcc-14), a separate translation unit from every C++ condition -- so it
// is always reached through a real out-of-line call, matching conditions
// B and C's noinline treatment (see docs/episode1_next_experiment_plan.md,
// "Preserve equivalent optimization opportunities").
//
// Six entry points: {real kalloc backend, fake fixed-slot backend} x
// {success path, explicit early-fail path}, all TIMED (no ledger, no
// injection -- this is what actually runs in the benchmark loop) -- plus
// one TEST-mode entry point (real backend, ledger-tracked, injectable)
// used only by the correctness harness, never by the timed loop.
#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"
#include "bench_resource_common.h"

extern void* fake_alloc(void);
extern void  fake_free(void*);
extern void* test_alloc_page(void);
extern void  test_free_page(void*);

// --- real kalloc backend, timed ---
int a_real_success(void** out1, void** out2) {
    void* p1 = kalloc();
    if (!p1) { *out1 = 0; *out2 = 0; return EP1_FAIL_FIRST; }
    EP1_TOUCH(p1);
    void* p2 = kalloc();
    if (!p2) { kfree(p1); *out1 = 0; *out2 = 0; return EP1_FAIL_SECOND; }
    EP1_TOUCH(p2);
    *out1 = p1;
    *out2 = p2;
    return EP1_OK;
}

int a_real_early_fail(void** out1, void** out2) {
    void* p1 = kalloc();
    if (!p1) { *out1 = 0; *out2 = 0; return EP1_FAIL_FIRST; }
    EP1_TOUCH(p1);
    void* p2 = kalloc();
    if (!p2) { kfree(p1); *out1 = 0; *out2 = 0; return EP1_FAIL_SECOND; }
    EP1_TOUCH(p2);
    kfree(p2);
    kfree(p1); // reverse acquisition order
    *out1 = 0;
    *out2 = 0;
    return EP1_EARLY_FAIL;
}

// --- fake fixed-slot backend, timed ---
int a_fake_success(void** out1, void** out2) {
    void* p1 = fake_alloc();
    if (!p1) { *out1 = 0; *out2 = 0; return EP1_FAIL_FIRST; }
    EP1_TOUCH(p1);
    void* p2 = fake_alloc();
    if (!p2) { fake_free(p1); *out1 = 0; *out2 = 0; return EP1_FAIL_SECOND; }
    EP1_TOUCH(p2);
    *out1 = p1;
    *out2 = p2;
    return EP1_OK;
}

int a_fake_early_fail(void** out1, void** out2) {
    void* p1 = fake_alloc();
    if (!p1) { *out1 = 0; *out2 = 0; return EP1_FAIL_FIRST; }
    EP1_TOUCH(p1);
    void* p2 = fake_alloc();
    if (!p2) { fake_free(p1); *out1 = 0; *out2 = 0; return EP1_FAIL_SECOND; }
    EP1_TOUCH(p2);
    fake_free(p2);
    fake_free(p1);
    *out1 = 0;
    *out2 = 0;
    return EP1_EARLY_FAIL;
}

// --- real backend, TEST mode (ledger-tracked, injectable) ---
// force_early_fail selects the EP1_EARLY_FAIL branch after both
// acquisitions succeed, so all four correctness scenarios (first-alloc-fail
// via ledger_inject_fail_at(1), second-alloc-fail via
// ledger_inject_fail_at(2), explicit-early-fail via force_early_fail=1,
// success+handoff via neither) share one entry point per condition.
int a_test(void** out1, void** out2, int force_early_fail) {
    void* p1 = test_alloc_page();
    if (!p1) { *out1 = 0; *out2 = 0; return EP1_FAIL_FIRST; }
    EP1_TOUCH(p1);
    void* p2 = test_alloc_page();
    if (!p2) { test_free_page(p1); *out1 = 0; *out2 = 0; return EP1_FAIL_SECOND; }
    EP1_TOUCH(p2);
    if (force_early_fail) {
        test_free_page(p2);
        test_free_page(p1);
        *out1 = 0;
        *out2 = 0;
        return EP1_EARLY_FAIL;
    }
    *out1 = p1;
    *out2 = p2;
    return EP1_OK;
}
