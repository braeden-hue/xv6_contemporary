#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

// Phase 1.8 experiment driver v2: fixes from external review (2026-09-12) --
// (1) wait(0) could reap an already-exited background A job instead of the
//     just-forked B job, silently corrupting response-time samples for
//     later rounds once A starts exiting. Now loops until the pid actually
//     forked for B is the one reaped.
// (2) NSHORT raised so P99 is a real percentile, not just the sample max
//     (with NSHORT=20, index 19 is just the maximum).
// (3) A jobs are no longer killed early -- they run to natural completion,
//     so their completion tick is a real throughput/progress measurement
//     instead of a binary finished/not-finished signal.

#define NLONG          4
#define LONG_ITERS     600000000
#define NSHORT         100
#define SHORT_ITERS    100000000
#define DEADLINE_TICKS 15   // response-time target for B, tunable

static void
longcpu_body(int id)
{
  volatile int dummy = 0;
  for (int i = 0; i < LONG_ITERS; i++)
    dummy += i;
  printf("[A pid=%d id=%d] done at tick=%d\n", getpid(), id, uptime());
}

static void
short_body(void)
{
  setpriority(1);
  volatile int dummy = 0;
  for (int i = 0; i < SHORT_ITERS; i++)
    dummy += i;
}

// wait() reaps whichever child happens to be ZOMBIE first (kernel/proc.c's
// kwait() just scans proc[] in order) -- not necessarily `target`. With A
// jobs as siblings of the child we're actually timing, an A exiting at the
// wrong moment silently steals the reap. Loop until we get `target` back;
// anything else reaped along the way is a background A finishing (it
// already printed its own completion line, nothing else to do with it).
static void
wait_for(int target)
{
  int got;
  do {
    got = wait(0);
  } while (got != target && got != -1);
}

int
main(void)
{
  int long_pids[NLONG];
  int response[NSHORT];
  int miss = 0;

  printf("[latencytest] starting %d background A jobs at tick=%d\n", NLONG, uptime());
  for (int i = 0; i < NLONG; i++) {
    int pid = fork();
    if (pid == 0) {
      longcpu_body(i);
      exit(0);
    }
    long_pids[i] = pid;
  }

  for (int i = 0; i < NSHORT; i++) {
    int t0 = uptime();
    int pid = fork();
    if (pid == 0) {
      short_body();
      exit(0);
    }
    wait_for(pid);
    response[i] = uptime() - t0;
    if (response[i] > DEADLINE_TICKS)
      miss++;
  }

  // NSHORT is small enough that plain insertion sort is fine for P50/P99.
  for (int i = 1; i < NSHORT; i++) {
    int key = response[i], j = i - 1;
    while (j >= 0 && response[j] > key) {
      response[j + 1] = response[j];
      j--;
    }
    response[j + 1] = key;
  }

  printf("[B] response times (ticks, sorted):");
  for (int i = 0; i < NSHORT; i++)
    printf(" %d", response[i]);
  printf("\n");

  int p50_idx = NSHORT / 2;
  int p99_idx = (NSHORT * 99) / 100;
  if (p99_idx >= NSHORT)
    p99_idx = NSHORT - 1;
  printf("[B] p50=%d p99=%d deadline=%d miss=%d/%d\n",
         response[p50_idx], response[p99_idx], DEADLINE_TICKS, miss, NSHORT);

  printf("[latencytest] B done at tick=%d, waiting for A to finish naturally...\n", uptime());
  for (int i = 0; i < NLONG; i++)
    wait_for(long_pids[i]);
  printf("[latencytest] all A done at tick=%d\n", uptime());

  exit(0);
}
