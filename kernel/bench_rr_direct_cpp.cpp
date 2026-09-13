// SCHED-EPISODE1-ABC-01: condition B, out-of-line variant. The SAME
// kernel/sched_rr.hpp RR policy object used everywhere else in this
// project, but reached through a __attribute__((noinline)) wrapper defined
// in its own translation unit -- so it CANNOT be inlined into
// kernel/bench.cpp's caller, matching condition A's out-of-line C function
// exactly. This is the fairness fix the design calls for: comparing an
// inlined C++ template call (the existing rr_static/rr_static_opaque
// numbers) against an out-of-line C function would mix "abstraction cost"
// with "which side happened to get inlined" -- two different questions.
// This file isolates the first question alone.
extern "C" {
#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
}

#include "sched_rr.hpp"   // same policy type as everywhere else, byte-for-byte

static RR g_cpp_direct_policy;

extern "C" __attribute__((noinline))
struct proc* cpp_rr_pick_next_noinline(struct proc* procs, int n) {
    return g_cpp_direct_policy.pick_next(procs, n);
}

extern "C" void cpp_rr_direct_reset_last(void) {
    g_cpp_direct_policy.last = -1;
}

extern "C" int cpp_rr_direct_get_last(void) {
    return g_cpp_direct_policy.last;
}
