#pragma once
#include <concepts>

extern "C" {
#include "types.h"
#include "proc.h"
}

template<typename P>
concept SchedulerPolicy = requires(P policy, struct proc* procs, int n) {
    { policy.pick_next(procs, n) } -> std::same_as<struct proc*>;
};