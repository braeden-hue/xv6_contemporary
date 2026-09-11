#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

// Phase 1.8b: automated regression test for sched_stats()'s open_wait_ticks
// path -- Codex's Implementation Gate audit (2026-09-12) rated the manual
// `schedstat` smoke test INSUFFICIENT_EVIDENCE specifically for this
// branch: closed-interval correctness (verified manually -- run_ticks==
// wait_ticks_total for two evenly-RR-alternating procs, exactly as
// expected) doesn't itself validate the `state==RUNNABLE` branch of
// sys_sched_stats(), and a human typing `schedstat <pid>` at the shell
// can't reliably land inside a single-tick wait window (three manual
// attempts all read open_wait_ticks=0).
//
// This test removes the human from the loop entirely: fork CPU-bound
// Normal-priority workers, then repeatedly query them from a THIRD-and-
// -up process (this one) that is itself part of the same RR rotation --
// deliberately no priority=1 (LatencySensitive) anywhere, so there's no
// starvation/deadlock risk the way a monopolizing LS proc would create
// (see kernel/sched_priority.hpp's should_preempt(): a LatencySensitive
// proc is never preempted by this policy, so making the sampler itself
// compete as Normal, in the same RR pool as the workers, is what
// synchronizes its own dispatch with the target's wait/run cycle instead
// of being an unrelated, asynchronous human keystroke).
//
// v1 of this test forked exactly 2 workers and queried only one -- every
// one of 40 samples read open_wait_ticks=0 (2026-09-12 first run). Root
// cause (not a kernel bug, confirmed via the closed-interval numbers
// being exactly right -- wait_ticks_total==2*run_ticks, wait_ticks_max
// pinned at 2, exactly what 3-way 1-tick RR predicts): the global `ticks`
// counter only advances on a timer IRQ, and mark_runnable() stamps
// sched_ready_tick from that same just-bumped value inside that same IRQ,
// right before dispatch hands the CPU to whoever's next. If the sampler
// happens to be the very next proc in RR order after the target it's
// querying, `ticks` hasn't moved again by the time the sampler samples
// (it won't, until the timer IRQ that ends the sampler's own tick) -- so
// "now - sched_ready_tick" reads exactly 0 for every sample taken during
// that entire tick, no matter how many times it's queried within it. This
// is specific to a sampler being RR-adjacent to its one target, not a
// property of open_wait_ticks in general (an external, non-participating
// observer, or a query of any *other* proc than the immediate RR
// predecessor, isn't subject to it). v2 (below) queries THREE workers
// every sample instead of one -- at most one of them can be the sampler's
// immediate RR predecessor, so the other two should show open_wait_ticks
// > 0.
#define NWORKERS 3
#define NSAMPLES 40

static void
spin_forever(void)
{
  volatile unsigned long i = 0;
  for(;;)
    i++;
}

int
main(void)
{
  int w[NWORKERS];
  uint64 out[4];
  int saw_open_positive = 0;
  int saw_run_progress = 0;
  uint64 first_run[NWORKERS];
  int i, j;

  for(j = 0; j < NWORKERS; j++){
    w[j] = fork();
    if(w[j] == 0)
      spin_forever();
  }

  for(i = 0; i < NSAMPLES; i++){
    for(j = 0; j < NWORKERS; j++){
      if(sched_stats(w[j], out) < 0){
        printf("statstest: FAIL -- pid %d disappeared mid-test (sample %d)\n", w[j], i);
        goto cleanup;
      }
      printf("sample %d worker %d (pid %d): run_ticks=%lu wait_ticks_total=%lu "
             "wait_ticks_max=%lu open_wait_ticks=%lu\n",
             i, j, w[j], out[0], out[1], out[2], out[3]);
      if(out[3] > 0)
        saw_open_positive = 1;
      if(i == 0)
        first_run[j] = out[0];
      else if(out[0] > first_run[j])
        saw_run_progress = 1;
    }
  }

cleanup:
  for(j = 0; j < NWORKERS; j++)
    kill(w[j]);
  for(j = 0; j < NWORKERS; j++)
    wait(0);

  if(saw_open_positive && saw_run_progress)
    printf("statstest: PASS -- open_wait_ticks was > 0 at least once, and "
           "run_ticks made progress (workers were actually running, not stuck)\n");
  else
    printf("statstest: FAIL -- saw_open_positive=%d saw_run_progress=%d\n",
           saw_open_positive, saw_run_progress);
  exit(0);
}
