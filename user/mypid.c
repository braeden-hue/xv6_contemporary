#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

// Phase 1.8b: trivial helper so `mypid &` gives you a concrete pid to feed
// into `schedstat` from the shell -- xv6's sh.c doesn't print a background
// job's pid the way bash does, so there was otherwise no way to learn it
// without adding this.
//
// Sleeps for a while after printing instead of exiting immediately: a
// backgrounded job (`cmd &`) gets reparented to init as soon as sh.c's
// intermediate forked child exits, and init's own wait() loop reaps
// orphaned zombies right away -- an instant-exit program would already be
// gone (UNUSED, not even queryable) before a human has time to type
// `schedstat <pid>` at the prompt. Pausing keeps it RUNNABLE/SLEEPING (a
// live, queryable proc) for a few seconds of real wall-clock time so
// there's an actual window to run schedstat against it.
int
main(void)
{
  printf("%d\n", getpid());
  pause(1000);
  exit(0);
}
