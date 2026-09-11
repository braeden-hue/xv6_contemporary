#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

// Phase 1.8b: CPU-bound helper for manually exercising sched_stats()'s
// open_wait_ticks path (the one mypid.c's pause()-based approach can't
// reach, since a SLEEPING proc is never RUNNABLE). Prints its own pid
// immediately, then busy-loops -- same LONG_ITERS scale as
// user/latencytest.c's A jobs (known from that experiment's raw results
// to run for well over 100 ticks under CPUS=1), so it stays
// RUNNABLE/RUNNING long enough to query mid-run.
//
// Manual test recipe (CPUS=1 so the two jobs actually compete for the one
// CPU instead of just running on separate cores):
//   $ burn &          # note the printed pid -- this one gets the CPU first
//   $ burn &          # note this pid -- it should sit RUNNABLE, waiting
//   $ schedstat <second pid>   # open_wait_ticks should be > 0
// 10x latencytest.c's LONG_ITERS -- 600M finished too fast under plain
// QEMU TCG (no -icount) for a human to type a query before it exited and
// got reaped, per the 2026-09-12 manual test session. This is purely for
// giving a person a typing window; not a scientifically meaningful scale.
#define LONG_ITERS 6000000000UL

int
main(void)
{
  volatile unsigned long dummy = 0;
  unsigned long i;   // must be wide enough for LONG_ITERS -- `int i` would
                      // overflow (UB) partway through a 6-billion-iteration
                      // count, since INT_MAX is ~2.1 billion

  printf("%d\n", getpid());
  for(i = 0; i < LONG_ITERS; i++)
    dummy += i;
  exit(0);
}
