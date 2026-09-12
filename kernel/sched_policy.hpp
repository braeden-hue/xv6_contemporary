#pragma once
#include <concepts>

extern "C" {
#include "types.h"
#include "proc.h"
}

template<typename P>
concept SchedulerPolicy = requires(P policy, struct proc* procs, int n, struct proc* running) {
    { policy.pick_next(procs, n) }     -> std::same_as<struct proc*>;
    { policy.should_preempt(running) } -> std::same_as<bool>;  // called once per timer
                                                                // tick, for the proc
                                                                // currently RUNNING in
                                                                // user mode (Phase 1.8)
};