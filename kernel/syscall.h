// System call numbers
#define SYS_fork    1
#define SYS_exit    2
#define SYS_wait    3
#define SYS_pipe    4
#define SYS_read    5
#define SYS_kill    6
#define SYS_exec    7
#define SYS_fstat   8
#define SYS_chdir   9
#define SYS_dup    10
#define SYS_getpid 11
#define SYS_sbrk   12
#define SYS_pause  13
#define SYS_uptime 14
#define SYS_open   15
#define SYS_write  16
#define SYS_mknod  17
#define SYS_unlink 18
#define SYS_link   19
#define SYS_mkdir  20
#define SYS_close  21
#define SYS_mmap   22
#define SYS_munmap 23
#define SYS_setpriority 24   // Phase 1.8: 0=Normal, 1=LatencySensitive, explicit only
#define SYS_sched_stats 25   // Phase 1.8b: read back a pid's tick accounting (fairness measurement)
#define SYS_sched_budget_stats 26   // SCHED-EP2-BUDGET-01: policy-level (not per-pid) budget diagnostics
#define SYS_sched_span_dump 27      // SCHED-EP2-BUDGET-01 S11: dump the span-observation per-window table
