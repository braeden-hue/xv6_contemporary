#pragma once
// SCHED-EP2-BUDGET-01 S11 step 3: single on/off switch for the whole
// high-resolution span-observation pilot, shared by kernel/scheduler.cpp
// (the dispatch-span hook) and kernel/sysproc.c (the priority-change
// hook). Flip to 0 and rebuild for the OFF half of the OFF/ON pilot pair
// -- when 0, every call site this guards compiles to nothing (not just an
// empty function call), so the OFF build carries zero added instructions
// in the dispatch hot path, a true unperturbed baseline.
#define EP2_SPAN_OBSERVE_ENABLED 0
