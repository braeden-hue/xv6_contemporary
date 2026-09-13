#pragma once
// EP1-RESOURCE-01: minimal move-only resource owner (docs/episode1_next_experiment_plan.md).
// Deliberately small -- no exceptions, no reference counting, no RTTI, no
// general allocator framework.
//
// Revision (2026-09-13, per review): UniquePage/TestUniquePage/UniqueSlot
// were originally three hand-duplicated classes with the same shape. That
// meant testing TestUniquePage's failure paths did not actually verify
// UniquePage's own code -- only that a similarly-shaped class behaved the
// same way. Fixed by making them three instantiations of ONE template
// parameterized on the backend's acquire/free functions -- same compiled
// logic, only the linked-in functions differ. This is still a single small
// owner type, not a general framework: the template parameter list is
// exactly the two functions a backend needs, nothing more.
//
// Contract (unchanged):
//   - null/empty default state
//   - copy constructor and copy assignment DELETED
//   - move assignment DELETED in this first version (keeps the contract
//     small, per plan) -- only move CONSTRUCTION is provided, and it empties
//     the source
//   - noexcept destructor that releases the held resource, if any
//   - get() for borrowed (non-owning) access
//   - release() for explicit ownership handoff (caller becomes responsible)
//   - the only way to construct a non-empty instance is the static acquire()
//     factory -- there is deliberately no PUBLIC constructor that adopts an
//     arbitrary raw pointer, so a caller cannot accidentally wrap a pointer
//     it doesn't actually own.

extern "C" {
void* kalloc(void);
void  kfree(void*);
void* test_alloc_page(void);
void  test_free_page(void*);
void* fake_alloc(void);
void  fake_free(void*);
}

template<void* (*AcquireFn)(void), void (*FreeFn)(void*)>
class UniqueResource {
    void* ptr_ = nullptr;
    explicit UniqueResource(void* p) noexcept : ptr_(p) {}

public:
    UniqueResource() noexcept = default;
    UniqueResource(const UniqueResource&) = delete;
    UniqueResource& operator=(const UniqueResource&) = delete;
    UniqueResource& operator=(UniqueResource&&) = delete; // deleted in v1, per plan

    UniqueResource(UniqueResource&& other) noexcept : ptr_(other.ptr_) {
        other.ptr_ = nullptr;
    }
    ~UniqueResource() noexcept {
        if (ptr_) FreeFn(ptr_);
    }

    void* get() const noexcept { return ptr_; }
    void* release() noexcept {
        void* p = ptr_;
        ptr_ = nullptr;
        return p;
    }
    explicit operator bool() const noexcept { return ptr_ != nullptr; }

    static UniqueResource acquire() noexcept { return UniqueResource(AcquireFn()); }
};

// UniquePage: production RAII owner over the real physical-page allocator.
// Used by this experiment's TIMED entry points.
using UniquePage = UniqueResource<kalloc, kfree>;

// TestUniquePage: the SAME template, instantiated with the ledgered test
// allocator instead of raw kalloc/kfree -- same compiled logic as
// UniquePage, verified by construction rather than by duplication. Used
// ONLY by the correctness/mutation tests.
using TestUniquePage = UniqueResource<test_alloc_page, test_free_page>;

// UniqueSlot: same template again, backed by the fixed-slot fake allocator.
// Used only by the timed "mechanics-only" measurement.
using UniqueSlot = UniqueResource<fake_alloc, fake_free>;
