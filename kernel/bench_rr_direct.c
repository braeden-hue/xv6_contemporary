// SCHED-EPISODE1-ABC-01: condition A -- the SAME RR search written directly
// in C, built by the C compiler ($(CC) = gcc-14), not the C++ compiler.
// This is a deliberate .c translation unit so kernel/bench.cpp (C++, gcc's
// sibling g++-14) can never inline across the TU boundary into it -- it is
// always reached through a real out-of-line call, exactly like condition
// C (the existing function-pointer contrast). See kernel/bench.cpp for
// condition B's matching out-of-line C++ wrapper and the fairness rationale
// (both A and B-out-of-line must get the SAME optimization opportunity --
// i.e. none, since neither can be inlined into the caller).
//
// Search order, `last` update, and wrap are copied verbatim from
// kernel/sched_rr.hpp's RR::pick_next -- only the "policy object with a
// member" shape is translated to "free function with a file-scope static",
// since plain C has no member functions. `n` is taken as an ordinary
// parameter (never a compile-time constant here, so this is naturally the
// "opaque n" condition Phase 1 already established as the confound-free
// one -- no separate opaque/non-opaque variant needed for A).
#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"

static int c_rr_last = -1;

struct proc* c_rr_pick_next(struct proc* procs, int n) {
    for (int i = 1; i <= n; i++) {          // <= n: must re-check `last` itself too
        int idx = (c_rr_last + i) % n;      // %, not / -- wrap around circularly
        struct proc* p = &procs[idx];
        if (p->state == RUNNABLE) {
            c_rr_last = idx;
            return p;
        }
    }
    return 0;
}

void c_rr_reset_last(void) {
    c_rr_last = -1;
}

int c_rr_get_last(void) {
    return c_rr_last;
}
