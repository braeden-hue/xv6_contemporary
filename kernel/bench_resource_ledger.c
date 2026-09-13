// EP1-RESOURCE-01: shared correctness infrastructure for the A/B/C
// two-page-ownership experiment (docs/episode1_next_experiment_plan.md).
// Plain C, deliberately: this is test-only harness code used identically by
// all three conditions' *test-mode* entry points, so it must not itself bias
// the A-vs-B-vs-C comparison. It is NEVER linked into any timed path (see
// bench_resource_harness.cpp's separate real/fake production entry points,
// which call kalloc()/kfree() or fake_alloc()/fake_free() directly with no
// ledger and no injection).
//
// test_alloc_page()/test_free_page() wrap the real kalloc()/kfree() with:
//   - injectable failure (fail the Nth call instead of touching kalloc),
//   - a small ledger recording which pointers are currently "test-owned",
//   - duplicate/unowned-free detection BEFORE the real kfree() is called
//     (so a mutation-test bug can never corrupt the real free list),
//   - an ORDERED EVENT LOG (added 2026-09-13 per review: the ledger
//     originally only checked "is this pointer currently live", not "did
//     alloc/free happen in the right order" -- release-order bugs could
//     pass undetected). The check_* helpers below verify LIFO release
//     structurally (last two frees are the reverse of the first two
//     allocs) rather than against a hardcoded expected pointer value,
//     since failure-path pointers are never returned to the caller.
#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

#define LEDGER_MAX 8
#define LEDGER_EVENT_MAX 16

#define EVENT_ALLOC 1
#define EVENT_FREE 2

static void* ledger_slots[LEDGER_MAX];
static int ledger_count;
static int ledger_dup_or_unowned; // sticky: set once, never auto-cleared except by ledger_reset()
static int ledger_fail_at_call;   // 1-based index of the alloc call to fail; 0 = never
static int ledger_call_counter;

static int ledger_event_kind_arr[LEDGER_EVENT_MAX];
static void* ledger_event_ptr_arr[LEDGER_EVENT_MAX];
static int ledger_event_count;

void ledger_reset(void) {
    ledger_count = 0;
    ledger_dup_or_unowned = 0;
    ledger_fail_at_call = 0;
    ledger_call_counter = 0;
    ledger_event_count = 0;
}

void ledger_inject_fail_at(int call_index) {
    ledger_fail_at_call = call_index;
}

static void ledger_record_event(int kind, void* p) {
    if (ledger_event_count < LEDGER_EVENT_MAX) {
        ledger_event_kind_arr[ledger_event_count] = kind;
        ledger_event_ptr_arr[ledger_event_count] = p;
        ledger_event_count++;
    }
}

void* test_alloc_page(void) {
    ledger_call_counter++;
    if (ledger_fail_at_call != 0 && ledger_call_counter == ledger_fail_at_call) {
        return 0; // simulated allocator failure -- never touches the real kalloc(), no event recorded
    }
    void* p = kalloc();
    if (p) {
        if (ledger_count < LEDGER_MAX) {
            ledger_slots[ledger_count] = p;
            ledger_count++;
        }
        ledger_record_event(EVENT_ALLOC, p);
    }
    return p;
}

static int ledger_index_of(void* p) {
    for (int i = 0; i < ledger_count; i++) {
        if (ledger_slots[i] == p) return i;
    }
    return -1;
}

void test_free_page(void* p) {
    if (!p) return; // freeing null is a documented no-op, same as kfree's callers assume
    int idx = ledger_index_of(p);
    if (idx < 0) {
        // Duplicate free (already removed from the ledger) or a pointer this
        // harness never allocated. Flag it and refuse -- do NOT call kfree()
        // on it, and do NOT record an event (this is the bug being caught,
        // not a real free).
        ledger_dup_or_unowned = 1;
        return;
    }
    ledger_slots[idx] = ledger_slots[ledger_count - 1];
    ledger_count--;
    ledger_record_event(EVENT_FREE, p);
    kfree(p);
}

int ledger_leaked_count(void) {
    return ledger_count; // pages still marked test-owned = leaked, if the caller believes it's done
}

int ledger_duplicate_or_unowned_detected(void) {
    return ledger_dup_or_unowned;
}

void ledger_reclaim_all(void) {
    // Test-only cleanup after an intentional-leak mutation scenario: return
    // every still-tracked page to the real allocator so the leak doesn't
    // persist into the rest of boot, then clear the ledger.
    for (int i = 0; i < ledger_count; i++) {
        kfree(ledger_slots[i]);
    }
    ledger_count = 0;
}

int ledger_event_count_get(void) {
    return ledger_event_count;
}
int ledger_event_kind(int i) {
    if (i < 0 || i >= ledger_event_count) return 0;
    return ledger_event_kind_arr[i];
}
void* ledger_event_ptr(int i) {
    if (i < 0 || i >= ledger_event_count) return 0;
    return ledger_event_ptr_arr[i];
}

// --- Structural release-order checks, reused by every scenario instead of
// hardcoding expected pointer values (failure paths never expose their
// pointers to the caller, so there is no external "expected" value to
// compare against -- these checks instead verify the RELATIONSHIP between
// the recorded events). ---

// Expects exactly: ALLOC(p_a), ALLOC(p_b), FREE(p_b), FREE(p_a) -- i.e. the
// two resources acquired in order p_a then p_b were released in the
// reverse order p_b then p_a (LIFO). Used for both the "success" case
// (after the harness's own out2-then-out1 cleanup) and the "early_fail"
// case (freed internally by the entry point in reverse order already).
int ledger_check_two_resource_reverse_release(void) {
    if (ledger_event_count != 4) return 0;
    if (ledger_event_kind_arr[0] != EVENT_ALLOC) return 0;
    if (ledger_event_kind_arr[1] != EVENT_ALLOC) return 0;
    if (ledger_event_kind_arr[2] != EVENT_FREE) return 0;
    if (ledger_event_kind_arr[3] != EVENT_FREE) return 0;
    void* p_a = ledger_event_ptr_arr[0];
    void* p_b = ledger_event_ptr_arr[1];
    if (ledger_event_ptr_arr[2] != p_b) return 0; // freed in reverse order: p_b first
    if (ledger_event_ptr_arr[3] != p_a) return 0; // then p_a
    return 1;
}

// Expects exactly: ALLOC(p_a), FREE(p_a) -- the second-allocation-fails
// scenario, where the only resource ever acquired is released once.
int ledger_check_one_alloc_one_free(void) {
    if (ledger_event_count != 2) return 0;
    if (ledger_event_kind_arr[0] != EVENT_ALLOC) return 0;
    if (ledger_event_kind_arr[1] != EVENT_FREE) return 0;
    return ledger_event_ptr_arr[0] == ledger_event_ptr_arr[1];
}

// Expects zero events -- the first-allocation-fails scenario.
int ledger_check_no_events(void) {
    return ledger_event_count == 0;
}

// --- Mutation scenarios: deliberately buggy manual-cleanup snippets used
// ONLY to verify the ledger oracle itself catches these mistakes. These are
// injected examples for testing the checker, not a claim that the real A/B/C
// code (which this file does not call) has these bugs. ---

void mutation_omit_free(void) {
    // Allocates one page and "forgets" to free it.
    test_alloc_page();
}

void mutation_duplicate_free(void) {
    // Allocates one page, frees it twice.
    void* p = test_alloc_page();
    test_free_page(p);
    test_free_page(p); // the bug: p is no longer test-owned here
}
