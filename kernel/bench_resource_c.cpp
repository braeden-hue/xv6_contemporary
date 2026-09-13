// EP1-RESOURCE-01, condition C: C++, RAII ownership via UniquePage /
// TestUniquePage / UniqueSlot (kernel/unique_page.hpp). Same observable
// contract as A/B (return codes, out-param null/non-null, allocation
// order, release order) -- but note what's ABSENT here compared to A/B:
// the "second acquisition failed" branch has no explicit free call for the
// first resource at all, and the early-fail branch has no free calls
// whatsoever. Both are handled by ~UniquePage() running at scope exit, in
// reverse declaration order (C++ guarantees this), which is exactly the
// "reverse acquisition order" release A/B have to write out by hand.
// extern "C" + noinline: out-of-line, plain-name entry points, matching A
// and B exactly.
#include "unique_page.hpp"
#include "bench_resource_common.h"

extern "C" __attribute__((noinline))
int c_real_success(void** out1, void** out2) {
    UniquePage a = UniquePage::acquire();
    if (!a) { *out1 = 0; *out2 = 0; return EP1_FAIL_FIRST; }
    EP1_TOUCH(a.get());
    UniquePage b = UniquePage::acquire();
    if (!b) { *out1 = 0; *out2 = 0; return EP1_FAIL_SECOND; } // a's destructor frees it here
    EP1_TOUCH(b.get());
    *out1 = a.release();
    *out2 = b.release();
    return EP1_OK;
}

extern "C" __attribute__((noinline))
int c_real_early_fail(void** out1, void** out2) {
    UniquePage a = UniquePage::acquire();
    if (!a) { *out1 = 0; *out2 = 0; return EP1_FAIL_FIRST; }
    EP1_TOUCH(a.get());
    UniquePage b = UniquePage::acquire();
    if (!b) { *out1 = 0; *out2 = 0; return EP1_FAIL_SECOND; }
    EP1_TOUCH(b.get());
    *out1 = 0;
    *out2 = 0;
    return EP1_EARLY_FAIL; // b then a destruct here, reverse declaration order
}

extern "C" __attribute__((noinline))
int c_fake_success(void** out1, void** out2) {
    UniqueSlot a = UniqueSlot::acquire();
    if (!a) { *out1 = 0; *out2 = 0; return EP1_FAIL_FIRST; }
    EP1_TOUCH(a.get());
    UniqueSlot b = UniqueSlot::acquire();
    if (!b) { *out1 = 0; *out2 = 0; return EP1_FAIL_SECOND; }
    EP1_TOUCH(b.get());
    *out1 = a.release();
    *out2 = b.release();
    return EP1_OK;
}

extern "C" __attribute__((noinline))
int c_fake_early_fail(void** out1, void** out2) {
    UniqueSlot a = UniqueSlot::acquire();
    if (!a) { *out1 = 0; *out2 = 0; return EP1_FAIL_FIRST; }
    EP1_TOUCH(a.get());
    UniqueSlot b = UniqueSlot::acquire();
    if (!b) { *out1 = 0; *out2 = 0; return EP1_FAIL_SECOND; }
    EP1_TOUCH(b.get());
    *out1 = 0;
    *out2 = 0;
    return EP1_EARLY_FAIL;
}

extern "C" __attribute__((noinline))
int c_test(void** out1, void** out2, int force_early_fail) {
    TestUniquePage a = TestUniquePage::acquire();
    if (!a) { *out1 = 0; *out2 = 0; return EP1_FAIL_FIRST; }
    EP1_TOUCH(a.get());
    TestUniquePage b = TestUniquePage::acquire();
    if (!b) { *out1 = 0; *out2 = 0; return EP1_FAIL_SECOND; }
    EP1_TOUCH(b.get());
    if (force_early_fail) {
        *out1 = 0;
        *out2 = 0;
        return EP1_EARLY_FAIL; // b then a destruct via the ledger, reverse order
    }
    *out1 = a.release();
    *out2 = b.release();
    return EP1_OK;
}

// --- C-only: move semantics. Not compared against A/B (per 2026-09-13
// clarification: A/B have no move concept to force into an equivalent
// shape). Uses the ledger to observe exactly one free happens once both
// the moved-from source and the move-constructed destination leave scope. ---
extern "C" {
void  ledger_reset(void);
int   ledger_leaked_count(void);
int   ledger_duplicate_or_unowned_detected(void);
}

extern "C" __attribute__((noinline))
int c_test_move_semantics(void) {
    ledger_reset();
    TestUniquePage a = TestUniquePage::acquire();
    if (!a) return -1; // setup failure, not part of the scenario itself
    void* raw = a.get();

    TestUniquePage b(static_cast<TestUniquePage&&>(a)); // move-construct

    int ok = 1;
    if (a.get() != nullptr) ok = 0;   // moved-from source must be emptied
    if (b.get() != raw) ok = 0;       // destination must hold the original pointer
    // `a` (empty, moved-from) and `b` (owns raw) both go out of scope here.
    // Caller checks ledger_leaked_count()==0 and !ledger_duplicate_or_unowned_detected()
    // afterward -- this only returns whether the observable pointer values
    // were correct during the move itself.
    return ok;
}
