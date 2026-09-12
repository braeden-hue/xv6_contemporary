#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

// Phase 1.8b: minimal CLI wrapper around sched_stats() (kernel/sysproc.c)
// for manual/shell-driven use -- e.g. `schedstat $!` right after
// backgrounding a job, or polled in a loop, while a fairness experiment is
// running in another shell. Deliberately does NOT itself drive a workload
// or fork anything -- that's user/latencytest.c's job. This just reads
// back whatever pid you point it at.
int
main(int argc, char **argv)
{
  uint64 out[4];   // [0]=run_ticks_total [1]=wait_ticks_total
                    // [2]=wait_ticks_max  [3]=open_wait_ticks (see
                    // kernel/sysproc.c's sys_sched_stats() comment)
  uint64 effective_max_wait;   // max(out[2], out[3]) -- see below
  int pid;

  if(argc != 2){
    fprintf(2, "usage: schedstat pid\n");
    exit(1);
  }
  pid = atoi(argv[1]);
  if(sched_stats(pid, out) < 0){
    fprintf(2, "schedstat: no such pid %d (already exited, or never existed)\n", pid);
    exit(1);
  }
  // effective worst-case wait so far: an in-progress wait hasn't been
  // closed into wait_ticks_max yet, so it must be compared in separately
  // (see kernel/sysproc.c's sys_sched_stats() comment).
  effective_max_wait = out[2] > out[3] ? out[2] : out[3];
  printf("pid %d: run_ticks=%lu wait_ticks_total=%lu wait_ticks_max=%lu open_wait_ticks=%lu effective_max_wait=%lu\n",
         pid, out[0], out[1], out[2], out[3], effective_max_wait);
  exit(0);
}
