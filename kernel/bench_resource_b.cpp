// EP1-RESOURCE-01, condition B: C++, manual acquire/release -- the exact
// same control flow and free calls as condition A (bench_resource_a.c),
// just compiled by $(CXX) (g++-14) instead of $(CC). No RAII object here on
// purpose: this isolates language/compiler differences from the ownership
// abstraction, which condition C (bench_resource_c.cpp) isolates alone.
// extern "C" + noinline: out-of-line, plain-name entry points, matching A
// and C exactly (see docs/episode1_next_experiment_plan.md).
extern "C" {
void* kalloc(void);
void  kfree(void*);
void* fake_alloc(void);
void  fake_free(void*);
void* test_alloc_page(void);
void  test_free_page(void*);
}
#include "bench_resource_common.h"

extern "C" __attribute__((noinline))
int b_real_success(void** out1, void** out2) {
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

extern "C" __attribute__((noinline))
int b_real_early_fail(void** out1, void** out2) {
    void* p1 = kalloc();
    if (!p1) { *out1 = 0; *out2 = 0; return EP1_FAIL_FIRST; }
    EP1_TOUCH(p1);
    void* p2 = kalloc();
    if (!p2) { kfree(p1); *out1 = 0; *out2 = 0; return EP1_FAIL_SECOND; }
    EP1_TOUCH(p2);
    kfree(p2);
    kfree(p1);
    *out1 = 0;
    *out2 = 0;
    return EP1_EARLY_FAIL;
}

extern "C" __attribute__((noinline))
int b_fake_success(void** out1, void** out2) {
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

extern "C" __attribute__((noinline))
int b_fake_early_fail(void** out1, void** out2) {
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

extern "C" __attribute__((noinline))
int b_test(void** out1, void** out2, int force_early_fail) {
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
