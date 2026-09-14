#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fcntl.h"
#include "user/user.h"

// Mixed Workload Response-Time Experiment
// (agent-management/projects/xv6_os_project/design/mixed_workload_experiment/, rev5,
// Design Gate ACCEPTED 2026-09-12; implementation per DECISIONS.md's MW-R5-01
// instruction). Replaces user/latencytest.c's experimental role -- see that DECISIONS.md
// entry and spec.md for the full methodology and its 5-round Design Gate revision history
// (measurement-window pollution, closed-loop-disguised-as-open-loop arrival, conflated
// policy effects, undefined acceptance criteria, a percentile-methodology gap, an FD-limit
// bug, a windowed-metric definition bug, and a real bug in an earlier pause()-based release
// design -- pause() re-competes for the CPU on every tick per kernel/sysproc.c's actual
// sys_pause(), so it cannot keep a task genuinely out of the RUNNABLE pool).
//
// Release mechanism: genuine blocking I/O (one dedicated pipe per task), not pause(). No
// kernel changes: this file only uses existing syscalls (fork/pipe/read/write/wait/
// setpriority/sched_stats/uptime/pause -- the last only for the fixed settle delay below,
// where re-competing every tick is irrelevant since nothing else is runnable yet at that
// point in the driver's own single-threaded setup phase).
//
// SHORT tasks call setpriority(1) unconditionally (spec.md §2: this equalizes per-task
// syscall overhead across all three ActivePolicy conditions being compared -- RR ignores
// the declaration but still pays the same syscall cost that SelectOnly/Preempt's short
// tasks do). LONG tasks never call setpriority -- Normal-priority background work in every
// condition. This single binary is unchanged across all three conditions; only
// kernel/scheduler.cpp's `using ActivePolicy = ...` line differs between builds (this
// project's established policy-swap precedent).
//
// Output is a stream of tagged, single-line, whitespace-separated records (never
// whitespace inside a field) so an offline aggregator can parse correctly even if two
// tasks' printf() output interleaves on the shared console fd -- a truncated/merged line
// fails the tag+field-count check instead of being silently misparsed (DECISIONS.md
// MW-R5-01 instruction 5).

// Which condition this build is testing -- a human-maintained label, kept
// in sync by hand with kernel/scheduler.cpp's `using ActivePolicy = ...`
// line (this project's established swap-and-rebuild precedent; there is no
// way for this user-space binary to introspect the kernel's compiled-in
// policy type). Printed once per run so a raw log is self-describing even
// out of context (Codex's 2nd-round Implementation Gate review, point 4).
#define MIXBENCH_CONDITION "SelectOnly"

// Bumped by hand whenever this file's measurement-affecting logic changes
// (not for comments/whitespace) -- lets a raw log be traced back to which
// revision of mixbench.c produced it without needing the source file or a
// git hash embedded at build time (Codex's Implementation Gate review,
// 2026-09-12, metadata point, requested twice).
// "v6" = the shared-pipe/small-record collection design, replacing the
// shared-FILE design (v5, abandoned: its escalating write_ticks and the
// inode-sleep-lock contention behind it were symptoms of going through the
// filesystem's log/disk path at all). "v7" (current) = MW-STAGE-C-01's
// pre-comparison fixes on top of v6 -- duplicate result records now sink
// collect_broken instead of being silently overwritten; the 4 cumulative
// sched_stats fields are explicitly range-checked against UINT32_MAX
// before being packed into the 32-bit wire record (not just assumed small
// because observed runs stay under ~100 ticks); the wire record's total
// size is verified at COMPILE time against PIPESIZE; and collection is now
// decoupled into two independent axes (MIXBENCH_QUIET_OUTPUT: printf vs
// shared-pipe; MIXBENCH_DEMOTE_ON_COLLECT: setpriority(0) before the
// collection step or not) so V/VD/QD can all be built from this one file.
// See DECISIONS.md's 2026-09-12 MW-STAGE-C-01 entry for the full history.
#define MIXBENCH_BUILD_TAG "mixbench_v7_stagec_arms"

// Stage C, output-mode comparison (user's 2026-09-12 direction after the
// Stage B redo): a completed SHORT task's own MIXBENCH_TASK printf() --
// issued immediately at self-exit, on the shared console fd -- can still
// physically compete with a LONG task's execution for CPU/console
// resources, the same category of interference already found and fixed
// for the driver's own MIXBENCH_CKPT print. This flag does NOT change what
// is measured (t_resume/t_complete/sched_stats are captured at the exact
// same points either way) -- only WHEN and HOW the record leaves the
// process:
//   0 (verbose, default, unchanged from all prior Stage A/B runs): each
//     task printf()s its own MIXBENCH_TASK line to console immediately at
//     self-exit, exactly as before -- this is the "기존 출력 방식" arm.
//   1 (quiet): each task instead writes ONE fixed-size 28-byte binary
//     record into a SINGLE SHARED PIPE (not a file -- see below), demoting
//     itself to Normal priority first (see MIXBENCH_TASK's collect_priority
//     field), then exits; the driver reads all of them back only after
//     every child is reaped, and prints the MIXBENCH_TASK lines itself
//     only then -- mirroring the already-proven MIXBENCH_CKPT defer
//     pattern. This is the "새 방식" arm.
//
// Design history (all in DECISIONS.md, 2026-09-12 entries, dated in
// order): a per-task dedicated pipe was tried first and rejected before
// implementation -- it needs the driver to hold BOTH a release-write-end
// AND a result-read-end per task for their full (non-overlapping-in-time)
// lifetimes, and because both spans start together (both must be created
// before that task's single fork() call) the driver's PEAK simultaneous fd
// count is 12+12=24, ~10 over NOFILE=16, regardless of ordering -- an
// unavoidable arithmetic fact given NTASKS=12, not a bug to work around.
// A single shared FILE was tried and built next (v5): it fits the fd
// budget (only 1 fd, closed+reopened rather than held both ends), but its
// per-task write() cost escalated (8->39 ticks, monotonically, across
// short tasks) and was traced to the inode's SLEEP-lock plus
// begin_op()/end_op()'s log-commit path -- a mechanism that lets other
// processes run while a writer waits, unlike a spinlock, and was found
// (via a targeted SelectOnly-vs-Preempt-vs-RR + setpriority(0)-before-
// collect diagnostic pair) to interact with LatencySensitive-first
// scheduling to starve the Normal-priority driver for 14-22 ticks at a
// stretch -- a genuine LOAD_TIMING_VIOLATION, not just a raw-cost problem.
//
// This (v6) design removes the filesystem log/disk path entirely by going
// back to a pipe, but SHARED (not per-task) and with a record small enough
// that the total across all 12 tasks provably never fills the pipe:
//   RESULT_REC_WORDS(7) * 4 bytes = 28 bytes/record * 12 tasks = 336 bytes,
//   comfortably under xv6's PIPESIZE=512 (kernel/pipe.c) -- so no writer
//   EVER hits pipewrite()'s "full, go to sleep" path. Read kernel/pipe.c
//   directly (not assumed): pipewrite() holds the pipe's SPINLOCK (which
//   disables this CPU's interrupts) for its entire byte-copy loop UNLESS
//   it has to sleep for space -- so a write that never needs to sleep
//   completes as one short, genuinely uninterruptible burst, which is what
//   makes "under 512 bytes total" an actual atomicity argument here rather
//   than a size-based assumption. This removes the log/disk path's cost
//   category, but pipe copy + syscall + exit cost still remains -- printed
//   explicitly as `output_mode=quiet` (never called "간섭 없음"/
//   interference-free, per the user's explicit instruction).
//
// MW-STAGE-C-01 (2026-09-12): V/VD/QD are two INDEPENDENT axes, not one
// flag -- verbose+demoted (VD) is a genuine new arm, separate from
// verbose+unchanged (V, the original all-prior-Stage-A/B behavior) and
// quiet+demoted (QD, what v6 built). Conflating "output path" and
// "collection priority" into a single toggle would make it impossible to
// tell a demotion effect from a collection-mechanism effect apart -- see
// the comparison rationale below.
//   MIXBENCH_QUIET_OUTPUT: 0=printf immediately at self-exit (verbose),
//     1=pack into the shared pipe, driver prints after full reap (quiet).
//   MIXBENCH_DEMOTE_ON_COLLECT: 0=stay at whatever priority the task
//     already has, 1=setpriority(0) right after body/stats are captured
//     (untouched) but before the collection step, for EVERY task
//     (LONG included -- already Normal, so this is a no-op priority-wise
//     for them, paid uniformly regardless, same "equalize syscall
//     overhead" principle as setpriority(1) above).
// The three arms actually built and compared (MW-STAGE-C-01):
//   V  = QUIET_OUTPUT 0, DEMOTE_ON_COLLECT 0 (unchanged baseline)
//   VD = QUIET_OUTPUT 0, DEMOTE_ON_COLLECT 1 (demotion's effect alone,
//        still printf-based -- isolates VD-V)
//   QD = QUIET_OUTPUT 1, DEMOTE_ON_COLLECT 1 (v6's shared-pipe design,
//        demotion held fixed -- isolates QD-VD as the collection-
//        mechanism change alone, with QD-V as the combined effect)
// (QUIET_OUTPUT 1 / DEMOTE_ON_COLLECT 0 -- "Q" -- compiles and works but
// is not part of this comparison; defined for completeness only.)
#define MIXBENCH_QUIET_OUTPUT 1
#define MIXBENCH_DEMOTE_ON_COLLECT 1

#if MIXBENCH_QUIET_OUTPUT
#define MIXBENCH_OUTPUT_MODE "quiet"
// Quiet mode closes fd 2 (see main()) to make room for the shared results
// pipe within NOFILE=16 -- every fprintf() that used to target stderr must
// go to fd 1 instead in this mode, or it would silently write into
// whatever unrelated fd happens to have been assigned slot 2 afterward.
// Verbose mode (V and VD alike) is untouched (fd 2 stays open) since
// neither needs the extra fd.
#define ERRFD 1
#else
#define MIXBENCH_OUTPUT_MODE "verbose"
#define ERRFD 2
#endif

#if MIXBENCH_DEMOTE_ON_COLLECT
#define MIXBENCH_COLLECT_PRIORITY "demoted"
#else
#define MIXBENCH_COLLECT_PRIORITY "unchanged"
#endif

#if MIXBENCH_QUIET_OUTPUT && MIXBENCH_DEMOTE_ON_COLLECT
#define MIXBENCH_MODE_LABEL "QD"
#elif !MIXBENCH_QUIET_OUTPUT && MIXBENCH_DEMOTE_ON_COLLECT
#define MIXBENCH_MODE_LABEL "VD"
#elif !MIXBENCH_QUIET_OUTPUT && !MIXBENCH_DEMOTE_ON_COLLECT
#define MIXBENCH_MODE_LABEL "V"
#else
#define MIXBENCH_MODE_LABEL "Q"
#endif

#define NTASKS 12
#define KIND_SHORT 0
#define KIND_LONG  1

// Fixed arrival schedule, indexed by fork order (task index == array index). Long tasks
// at index 0 and 6 so short tasks arrive both before, during, and after both of them are
// running. Not tuned further for this Stage A pilot -- spec.md leaves the exact
// interleaving an implementation choice, not something the design fixes; real spacing
// will be revisited once this pilot's raw tick-scale is known (design/mixed_workload_
// experiment/spec.md §7 Stage A/B process).
static const int g_kind[NTASKS]   = { KIND_LONG, KIND_SHORT, KIND_SHORT, KIND_SHORT,
                                       KIND_SHORT, KIND_SHORT, KIND_LONG, KIND_SHORT,
                                       KIND_SHORT, KIND_SHORT, KIND_SHORT, KIND_SHORT };
static const int g_offset[NTASKS] = { 0, 3, 6, 9, 12, 15, 18, 21, 24, 27, 30, 33 };

// Iteration counts for the simulated work between t_resume and t_complete. LONG matches
// the scale already established by user/latencytest.c and user/burn.c's raw results
// (well over 100 ticks under CPUS=1). SHORT is deliberately much smaller
// (interactive-sized); exact tuning is a Stage A pilot *output*, not a design input.
#define LONG_ITERS  600000000UL
#define SHORT_ITERS 2000000UL

// Fixed settle time after the fork loop, before the driver reads the common t0 -- an
// explicitly accepted approximation (spec.md §1 step 3), not a proof that every child has
// reached its blocking read(). Value itself is a Stage A pilot output too; this is a
// starting point.
#define SETTLE_TICKS 5

static void
die(const char *msg)
{
  fprintf(ERRFD, "mixbench: FATAL: %s\n", msg);
  exit(1);
}

// Busy-work body shared by both kinds. Unsigned arithmetic throughout, so a large
// iteration count can't trip signed-overflow UB (the lesson from user/burn.c earlier in
// this project; also matches kernel/scheduler.cpp's wrapping-uint32 precedent).
static void
do_work(unsigned long iters)
{
  volatile unsigned long dummy = 0;
  unsigned long j;
  for(j = 0; j < iters; j++)
    dummy += j;
}

// All timestamps are stored widened to uint64 (matching sched_stats()'s ABI and this
// project's printf convention, which requires %lu arguments to actually be uint64 --
// passing a narrower type through a variadic call would misalign the callee's va_arg
// reads), but every *difference* between two timestamps is computed via an explicit
// uint32 modular subtraction first (DECISIONS.md MW-R5-01 instruction 6), matching
// kernel/scheduler.cpp's already-established `now32 - (uint32)sched_ready_tick` pattern,
// before being widened back to uint64. uptime() itself returns a 32-bit tick count
// reinterpreted through user.h's `int` return type; casting through (uint32) recovers the
// correct bit pattern regardless of that signed/unsigned mismatch.
static uint64
now_tick(void)
{
  return (uint64)(uint32)uptime();
}

static uint64
tick_diff(uint64 later, uint64 earlier)
{
  uint32 d = (uint32)later - (uint32)earlier;
  return (uint64)d;
}

#if MIXBENCH_QUIET_OUTPUT
// Shared-pipe wire format -- ONE 7-word (28-byte) record per task, written
// in a single write() call. Fields the driver already knows independently
// (pid -- it forked every child itself; kind -- g_kind[] is a compile-time
// table) are deliberately NOT transmitted, keeping the record small enough
// that all 12 fit in the pipe with room to spare (12*28=336 of 512 bytes,
// see the file-header comment for why that matters to pipewrite()'s
// atomicity).
//   word[0]: bits[3:0]=idx(0-11), bit4=stats_ok, bit5=task_failed, rest 0
//   word[1]: t_resume    (uint32 -- same 32-bit tick convention as
//                          now_tick()/tick_diff() elsewhere in this file)
//   word[2]: t_complete  (uint32)
//   word[3]: run_ticks   (uint32 -- justified, not an unexamined narrowing:
//                          this experiment's own t_end is ~90-100 ticks in
//                          every observed run, so a cumulative counter
//                          bounded by t_end cannot approach 2^32; the
//                          kernel's own sched_stats() ABI stays uint64,
//                          this is only this wire format's choice)
//   word[4]: wait_total  (uint32, same justification)
//   word[5]: wait_max    (uint32, same justification)
//   word[6]: open_wait   (uint32, same justification)
#define RESULT_REC_WORDS 7

// Compile-time (not just documented) proof that every task's record fits
// in xv6's pipe buffer with room to spare -- MW-STAGE-C-01's requirement
// that the total wire capacity be checked against PIPESIZE mechanically,
// not left as a comment someone could silently invalidate by bumping
// RESULT_REC_WORDS or NTASKS later. A negative array size is a portable
// pre-C11 compile-time assertion trick: if the condition is false, this
// typedef itself fails to compile.
typedef char mixbench_pipe_capacity_check
    [(RESULT_REC_WORDS * 4 * NTASKS <= 512) ? 1 : -1];

// Reads exactly `n` bytes from `fd`, loop-accumulating over possibly-partial
// read() returns (piperead() only guarantees returning AT MOST the
// requested count, not exactly it -- a naive single read() call could
// silently hand back a truncated record). Returns 0 on success, -1 on error
// or EOF before `n` bytes were seen (never silently accepts a short record).
static int
readn(int fd, void *buf, int n)
{
  char *p = (char *)buf;
  int got = 0;
  while(got < n){
    int r = read(fd, p + got, n - got);
    if(r <= 0)
      return -1;
    got += r;
  }
  return 0;
}

// Packs one task's result into the 28-byte wire format and writes it in a
// SINGLE write() call. Per kernel/pipe.c's actual pipewrite(): as long as
// the pipe never has to block for space (guaranteed here -- see the
// file-header comment), this whole write() completes under one
// spinlock-held, interrupt-disabled pass with no other writer able to
// interleave a single byte into the middle of it, so records from
// different tasks can never merge at the byte level regardless of arrival
// order. Returns 1 iff write() returned exactly sizeof(rec); the caller
// must fold a 0 return into task_failed/exit status, not silently accept a
// short or failed write as a successful result (MW-R5-01 instruction 4).
static int
send_result(int fd, int idx, int stats_ok, int task_failed,
            uint64 t_resume, uint64 t_complete, uint64 run_ticks,
            uint64 wait_total, uint64 wait_max, uint64 open_wait)
{
  uint32 rec[RESULT_REC_WORDS];
  rec[0] = ((uint32)idx & 0xF) |
           (stats_ok ? (1u << 4) : 0) |
           (task_failed ? (1u << 5) : 0);
  rec[1] = (uint32)t_resume;
  rec[2] = (uint32)t_complete;
  rec[3] = (uint32)run_ticks;
  rec[4] = (uint32)wait_total;
  rec[5] = (uint32)wait_max;
  rec[6] = (uint32)open_wait;

  int n = write(fd, rec, sizeof(rec));
  int ok = (n == (int)sizeof(rec));
  if(!ok)
    fprintf(ERRFD, "mixbench: idx=%d results write() returned %d, expected %d\n",
            idx, n, (int)sizeof(rec));
  return ok;
}
#endif

int
main(void)
{
  int release_fd[NTASKS][2];
  int pid[NTASKS];          // -1 until that index's fork() actually succeeds
  int ncreated = 0;
  int i;
  uint64 t0;
  // SCHED-EP2-BUDGET-01 (2026-09-14 review): before/after snapshot, not a
  // single end-of-run cumulative-since-boot read -- the three counters are
  // monotonic since kernel boot, so reading them only once at the end would
  // conflate this workload's contribution with whatever ran before it.
  // budget_start_ok tracks whether the START query itself succeeded; a
  // failure there (or at the end) invalidates the run outright (see
  // run_valid below) rather than just silently omitting the print line.
  uint64 budget_start[3];
  int budget_start_ok;

  for(i = 0; i < NTASKS; i++)
    pid[i] = -1;

#if MIXBENCH_QUIET_OUTPUT
  // Close stdin AND stderr (not just stdin) -- the shared results pipe
  // costs the driver 1 fd held from before the fork loop until after reap
  // (its read end), on top of the 12 release-write-ends the existing
  // design already holds throughout the fork loop; the fork loop's own
  // peak (creating the LAST release pipe, both ends transiently open)
  // already sits at 3(std)+11(prior release-writes)+2(transient)=16 with
  // no slack at all, so adding even 1 more fd needs 2 slots freed, not 1
  // (see docs/bench/stageb-adjacent design note in DECISIONS.md's
  // 2026-09-12 "공유 파이프" entry for the full fd lifetime table). Every
  // fprintf() in this file uses ERRFD (=1 here) instead of a literal fd
  // number, so closing fd 2 doesn't silently misdirect error output.
  close(0);
  close(2);
  int result_fd[2];
  if(pipe(result_fd) < 0)
    die("results pipe() failed");
#endif

  for(i = 0; i < NTASKS; i++){
    if(pipe(release_fd[i]) < 0){
      fprintf(ERRFD, "mixbench: pipe() failed at index %d, stopping creation\n", i);
      break;
    }

    int f = fork();
    if(f < 0){
      // fork() itself failed -- close this index's pipe (never handed to
      // anyone) and stop creating further tasks. Instruction 4: don't
      // silently continue as if nothing happened.
      close(release_fd[i][0]);
      close(release_fd[i][1]);
      fprintf(ERRFD, "mixbench: fork() failed at index %d, stopping creation\n", i);
      break;
    }

    if(f == 0){
      // Child. fork() copies the whole fd table, so this child also holds
      // copies of every earlier release pipe's write end (indices 0..i-1) --
      // close those too, not just this pipe's own write end (spec.md §1
      // step 1's FD-hygiene requirement; DECISIONS.md MW-R5-01 instruction 3).
      int k;
      for(k = 0; k < i; k++)
        close(release_fd[k][1]);
      close(release_fd[i][1]);
#if MIXBENCH_QUIET_OUTPUT
      // This task only ever WRITES its own result -- the read end is the
      // driver's alone. Same FD-hygiene principle as the release pipes.
      close(result_fd[0]);
#endif

      if(g_kind[i] == KIND_SHORT){
        if(setpriority(1) < 0)
          die("setpriority() failed");
      }

      char b;
      int n = read(release_fd[i][0], &b, 1);
      // t_resume is captured IMMEDIATELY after read() returns -- before
      // close() or the success check -- per Codex's Implementation Gate
      // review (2026-09-12): recording it after an extra syscall would let
      // that syscall's own cost leak into the measured release-to-resume
      // interval. A failed read (n != 1) still gets a t_resume timestamp
      // for the record, but is reported as a hard failure below (not
      // silently treated as a valid response-time sample).
      uint64 t_resume = now_tick();
      close(release_fd[i][0]);

      if(n != 1){
        fprintf(ERRFD, "mixbench: idx=%d release read() failed or got EOF (n=%d)\n", i, n);
#if MIXBENCH_QUIET_OUTPUT
        send_result(result_fd[1], i, /*stats_ok=*/0, /*task_failed=*/1,
                    t_resume, t_resume, 0, 0, 0, 0);
#else
        printf("MIXBENCH_TASK idx=%d pid=%d kind=%d t_resume=%lu t_complete=%lu "
               "stats_ok=0 source=self_at_exit collect_mode=%s run_ticks=0 "
               "wait_total=0 wait_max=0 open_wait=0 task_failed=1\n",
               i, getpid(), g_kind[i], t_resume, t_resume, MIXBENCH_OUTPUT_MODE);
#endif
        exit(1);
      }

      do_work(g_kind[i] == KIND_LONG ? LONG_ITERS : SHORT_ITERS);

      uint64 t_complete = now_tick();

      uint64 out[4];
      int have_stats = (sched_stats(getpid(), out) == 0);

      // MW-STAGE-C-01 pre-comparison fix: the 4 cumulative sched_stats
      // fields are uint64 in the kernel ABI but get packed into 32-bit
      // wire-record words (both the quiet-pipe record and, historically,
      // this project's printf %lu-as-uint64 convention are fine for
      // PRINTING a uint64 -- the risk is specific to actually NARROWING to
      // uint32 for the pipe record). Checking that this run's observed
      // scale (~100 ticks) stays under UINT32_MAX is not itself a safety
      // proof for ALL possible runs -- so each field is checked explicitly
      // here, and a run whose stats exceed the checked range is treated as
      // a genuine failure (not silently truncated), regardless of arm.
      int stats_in_range = 1;
      if(have_stats){
        int k;
        static const char *field_names[4] =
            { "run_ticks", "wait_total", "wait_max", "open_wait" };
        for(k = 0; k < 4; k++){
          if(out[k] > 0xFFFFFFFFUL){
            fprintf(ERRFD, "mixbench: idx=%d field=%s value=%lu exceeds "
                    "UINT32_MAX, cannot pack into the wire record\n",
                    i, field_names[k], out[k]);
            stats_in_range = 0;
          }
        }
      }
      int final_stats_ok = have_stats && stats_in_range;

      // One tagged record per task, output deferred until here (after every
      // timestamp of interest is already captured in local variables) --
      // spec.md §1's output-timing rule. stats_ok=0 is reported explicitly
      // (unavailable -- either the sched_stats() call itself failed, or it
      // succeeded but a field was out of the wire format's safe range),
      // not silently defaulted to zero as if it were a real reading
      // (DECISIONS.md MW-R5-01 instruction 7). source=self_at_exit marks
      // these run_ticks/wait_total/wait_max/open_wait values as a snapshot
      // this task took of itself right before exiting -- NOT a live
      // driver-side query at a known common checkpoint tick (that's what
      // MIXBENCH_CKPT's source=live_query lines are for) -- so an
      // aggregator never conflates the two (Codex's review, point 4).
#if MIXBENCH_DEMOTE_ON_COLLECT
      // Demote to Normal priority now -- AFTER t_complete/out[] above are
      // already captured (so THIS task's own measurement is untouched),
      // BEFORE the collection step below. Independent of output path (V
      // and QD both demote in this comparison; VD is exactly V plus this
      // one call, isolating the demotion's own effect). This can still
      // change what OTHER tasks experience while this one collects --
      // never claimed to be side-effect-free for the whole run, only for
      // this task's own already-captured values.
      if(setpriority(0) < 0)
        die("setpriority(0) before collect failed");
#endif

#if MIXBENCH_QUIET_OUTPUT
      // Quiet arm: no printf, no console touch at all here -- pack the
      // record and hand it to the shared results pipe. The driver prints
      // the MIXBENCH_TASK line for this task only after every child (this
      // one included) has been reaped, using its OWN already-known pid[i]/
      // g_kind[i] plus these self-reported values (see the collection step
      // after the reap loop, below).
      //
      // A results write() that didn't fully land is itself a real failure
      // -- this task's own measurement was captured correctly, but if it
      // never reaches the driver, the driver would otherwise report it as
      // silently "missing" rather than "measured but lost in transit".
      // Folding it into task_failed (and this process's exit status) makes
      // that distinction visible in MIXBENCH_DONE's all_exit_ok, matching
      // this project's established rigor for every other collection path
      // (MW-R5-01 instruction 4).
      int results_write_ok = send_result(
          result_fd[1], i, final_stats_ok, final_stats_ok ? 0 : 1,
          t_resume, t_complete,
          final_stats_ok ? out[0] : 0, final_stats_ok ? out[1] : 0,
          final_stats_ok ? out[2] : 0, final_stats_ok ? out[3] : 0);
      if(!results_write_ok)
        exit(1);
#else
      printf("MIXBENCH_TASK idx=%d pid=%d kind=%d t_resume=%lu t_complete=%lu "
             "stats_ok=%d source=self_at_exit collect_mode=%s run_ticks=%lu "
             "wait_total=%lu wait_max=%lu open_wait=%lu task_failed=%d\n",
             i, getpid(), g_kind[i], t_resume, t_complete,
             final_stats_ok, MIXBENCH_OUTPUT_MODE,
             final_stats_ok ? out[0] : 0,
             final_stats_ok ? out[1] : 0,
             final_stats_ok ? out[2] : 0,
             final_stats_ok ? out[3] : 0,
             final_stats_ok ? 0 : 1);
#endif

      // A task whose own final sched_stats query failed OR came back
      // out-of-range exits non-zero so the parent's per-child exit-status
      // check (below) surfaces it as a real failure, not a silently-
      // accepted PASS (Codex's review, point 3).
      exit(final_stats_ok ? 0 : 1);
    }

    // Parent: only needs the write end for later release.
    close(release_fd[i][0]);
    pid[i] = f;
    ncreated++;
  }

#if MIXBENCH_QUIET_OUTPUT
  // Every child that will ever exist has now been forked (and inherited
  // its own copy of the shared pipe's write end) -- the driver's own copy
  // is never written to, only read from (via result_fd[0], kept open).
  close(result_fd[1]);
#endif

  // Fixed settle time -- accepted approximation (see file header), not proof
  // that every child has reached its blocking read(). Nothing else this
  // driver cares about is runnable during this window (every child that
  // exists yet is already blocked on its own release pipe, or on its way
  // there), so pause()'s per-tick re-competition (the bug that sank the
  // earlier pause()-based *release* design) is irrelevant here -- there is
  // no release-timing claim being made about this specific wait.
  pause(SETTLE_TICKS);

  t0 = now_tick();
  // Start snapshot -- see the declaration comment above. Taken as close to
  // t0 as possible (after settle, before any task is released) so the
  // window this snapshot brackets matches t0..t_end as closely as the
  // syscall boundary allows.
  budget_start_ok = (sched_budget_stats(budget_start) == 0);
  // run=1: this binary does not loop over repeated in-kernel runs itself --
  // each QEMU boot is one run, and "run" here is a fixed placeholder for
  // Stage A's single pilot invocation (spec.md's N>=5-per-condition Stage B
  // repetition happens by rebooting and re-invoking this same binary N
  // times, not by this binary looping internally). A future revision could
  // take the run number as an argv, but this project's other CLI tools
  // (schedstat, burn, ...) don't currently pass argv into user programs
  // beyond argc/argv already supported by exec() -- left as a documented
  // gap, not silently implied to be handled (Codex's Implementation Gate
  // review, 2026-09-12, metadata point).
  printf("MIXBENCH_META condition=%s run=1 ntasks=%d nshort=%d nlong=%d "
         "long_iters=%lu short_iters=%lu settle_ticks=%d build_tag=%s "
         "output_mode=%s collect_priority=%s mode_label=%s\n",
         MIXBENCH_CONDITION, NTASKS, NTASKS - 2, 2,
         (uint64)LONG_ITERS, (uint64)SHORT_ITERS, SETTLE_TICKS,
         MIXBENCH_BUILD_TAG, MIXBENCH_OUTPUT_MODE, MIXBENCH_COLLECT_PRIORITY,
         MIXBENCH_MODE_LABEL);
  // Full schedule dumped explicitly, one line per task, so a raw log is
  // self-describing even without this source file open next to it (Codex's
  // review: "12개 offset을 로그에 남기고").
  for(i = 0; i < NTASKS; i++)
    printf("MIXBENCH_SCHEDULE idx=%d kind=%d offset=%d\n", i, g_kind[i], g_offset[i]);
  printf("MIXBENCH_T0 t0=%lu ncreated=%d settle_ticks=%d\n", t0, ncreated, SETTLE_TICKS);

  // Release schedule, in index order (spec.md §1 step 5) -- only release
  // indices that were actually created. No explicit kill() is needed for
  // any created-but-not-yet-released task: every created index is still
  // reached by this same loop (it only ever `continue`s past indices that
  // were never created), so nothing is left permanently blocked waiting for
  // a release that will never come.
  uint64 ck_a = t0;   // fallback if somehow nothing gets released at all
  int any_release_failed = 0;
  for(i = 0; i < NTASKS; i++){
    if(pid[i] < 0)
      continue;

    // Wraparound-safe wait: compare *elapsed* ticks since t0 (via the same
    // modular tick_diff() used everywhere else in this project) against
    // the offset, instead of comparing now_tick() to an absolute
    // `t0 + offset` target. Codex's Implementation Gate review
    // (2026-09-12): the earlier absolute-target form could busy-wait
    // forever if the global tick counter wraps past 2^32 between t0 being
    // read and this loop running (now_tick() would drop back to a small
    // value while `target` stayed a stale large one).
    while(tick_diff(now_tick(), t0) < (uint64)g_offset[i])
      ;   // coarse busy-wait; same category of tick-granularity
          // approximation already accepted for the settle time above and
          // the release write() itself (spec.md §1 step 5's commentary)

    uint64 target = t0 + (uint64)g_offset[i];   // for reporting only -- the
                                                 // wait loop above never
                                                 // compares against this
                                                 // directly (see above)
    uint64 t_release_actual = now_tick();
    char b = 1;
    int wrote = (write(release_fd[i][1], &b, 1) == 1);
    if(!wrote){
      any_release_failed = 1;
      fprintf(ERRFD, "mixbench: idx=%d release write() failed\n", i);
    }
    close(release_fd[i][1]);

    // write_ok is reported explicitly per record, not just to stderr --
    // Codex's review, point 3: a failed release must be visible to the
    // aggregator on the same structured line, not just a WARN a parser
    // might not be watching for.
    printf("MIXBENCH_RELEASE idx=%d intended=%lu actual=%lu jitter=%lu write_ok=%d\n",
           i, target, t_release_actual, tick_diff(t_release_actual, target), wrote);

    if(wrote)
      ck_a = t_release_actual;   // last successfully-released task's actual tick
  }

  // Driver-side sched_stats sampling of any LONG task still alive at ck_a
  // (spec.md §6, windowed fairness) -- best-effort; a task that has already
  // exited by ck_a is instead covered by its own self-reported MIXBENCH_TASK
  // line (source=self_at_exit -- NOT a live checkpoint read, the closest
  // available substitute, per spec.md §6's explicit handling of that case).
  // query_tick (the actual tick this sched_stats() call happened at) is
  // reported separately from ck_a (the reference checkpoint the windowed
  // metric is defined against) -- querying N long tasks in sequence takes
  // a few instructions each, so they are not all sampled at literally the
  // same instant as ck_a itself (Codex's review, point 4).
  //
  // The QUERY happens here (same timing as before -- right after the
  // release loop), but printing it is deferred until after every child is
  // reaped (see below): a print here could be interrupted mid-printf() by
  // a timer tick that hands the CPU to a still-running short task, which
  // then prints its OWN complete MIXBENCH_TASK line before this one
  // finishes -- corrupting one physical console line into an unparseable
  // merge of two records (found and fixed in verbose mode; quiet mode has
  // no per-task console output at all during the run, but the CKPT defer
  // is kept identical across both modes for consistency).
  uint64 ckpt_pid[NTASKS], ckpt_ck_a[NTASKS], ckpt_query_tick[NTASKS];
  uint64 ckpt_out[NTASKS][4];
  int ckpt_ok[NTASKS];       // 1 = sched_stats() succeeded for this idx
  int ckpt_present[NTASKS];  // 1 = this idx was queried at all (is LONG + created)
  for(i = 0; i < NTASKS; i++)
    ckpt_present[i] = 0;
  for(i = 0; i < NTASKS; i++){
    if(pid[i] < 0 || g_kind[i] != KIND_LONG)
      continue;
    ckpt_present[i] = 1;
    ckpt_pid[i] = pid[i];
    ckpt_ck_a[i] = ck_a;
    ckpt_query_tick[i] = now_tick();
    ckpt_ok[i] = (sched_stats(pid[i], ckpt_out[i]) == 0);
  }

#if MIXBENCH_QUIET_OUTPUT
  int task_present[NTASKS];
  int task_ok[NTASKS], task_failed_flag[NTASKS];
  uint64 task_t_resume[NTASKS], task_t_complete[NTASKS];
  uint64 task_run_ticks[NTASKS], task_wait_total[NTASKS];
  uint64 task_wait_max[NTASKS], task_open_wait[NTASKS];
  for(i = 0; i < NTASKS; i++)
    task_present[i] = 0;
  int ncollected = 0;
  int collect_broken = 0;
#endif

  // Reap exactly ncreated children -- plain wait() in a loop, recording every
  // returned pid and exit status, then verifying the returned set is
  // exactly the set of created pids (spec.md §4 -- fixes latencytest.c's
  // wait_for() pattern, which can silently mis-reap out of order since
  // kwait() always reaps whichever ZOMBIE appears first regardless of which
  // pid the caller asked for).
  {
    int reaped_pid[NTASKS];
    int reaped_status[NTASKS];
    int nreaped = 0;
    int j, k;

    for(j = 0; j < ncreated; j++){
      int st = -1;
      int r = wait(&st);
      if(r < 0){
        fprintf(ERRFD, "mixbench: WARN wait() returned error after %d of %d expected reaps\n",
                nreaped, ncreated);
        break;
      }
      reaped_pid[nreaped] = r;
      reaped_status[nreaped] = st;
      nreaped++;
    }

    uint64 t_end = now_tick();

#if MIXBENCH_QUIET_OUTPUT
    // Every child has been reaped, so every write into the shared pipe
    // (each one a single, never-blocked, lock-held burst -- see the
    // file-header comment) has already happened. Read all `ncreated`
    // records back now; the pipe's read end (result_fd[0]) has been open
    // continuously since before the fork loop, so unlike the file-based
    // design this replaced, there is no offset/lseek concern here at all --
    // a pipe is just a byte stream, and every byte written is still
    // sitting in it waiting to be read.
    for(j = 0; j < ncreated; j++){
      uint32 rec[RESULT_REC_WORDS];
      if(readn(result_fd[0], rec, sizeof(rec)) < 0){
        fprintf(ERRFD, "mixbench: results pipe readn() failed after %d of "
                "%d expected records\n", ncollected, ncreated);
        collect_broken = 1;
        break;
      }
      int idx = (int)(rec[0] & 0xF);
      if(idx < 0 || idx >= NTASKS){
        fprintf(ERRFD, "mixbench: results record with out-of-range idx=%d\n", idx);
        collect_broken = 1;
        break;
      }
      if(task_present[idx]){
        // MW-STAGE-C-01 pre-comparison fix: a duplicate record means the
        // wire stream is not trustworthy for this run (either a genuine
        // double-send, or evidence of the byte-stream misalignment risk
        // documented in send_result()'s header comment) -- sink the run
        // instead of silently keeping either copy.
        fprintf(ERRFD, "mixbench: duplicate result record for idx=%d -- "
                "run invalid\n", idx);
        collect_broken = 1;
        continue;
      }
      task_present[idx] = 1;
      task_ok[idx] = (int)((rec[0] >> 4) & 1);
      task_failed_flag[idx] = (int)((rec[0] >> 5) & 1);
      task_t_resume[idx] = rec[1];
      task_t_complete[idx] = rec[2];
      task_run_ticks[idx] = rec[3];
      task_wait_total[idx] = rec[4];
      task_wait_max[idx] = rec[5];
      task_open_wait[idx] = rec[6];
      ncollected++;
    }
    close(result_fd[0]);

    // Explicit per-instruction check (not just relying on the exit-status
    // path above): any task whose OWN self-reported record says it failed
    // (out-of-range stats, or the early release-read failure path) must
    // sink run_valid too, even if -- hypothetically -- its exit status
    // were somehow still 0. Redundant with all_exit_ok below in the normal
    // case, but explicit rather than assumed.
    for(i = 0; i < NTASKS; i++){
      if(pid[i] >= 0 && task_present[i] && task_failed_flag[i])
        collect_broken = 1;
    }

    // Deferred MIXBENCH_TASK lines (quiet mode's whole point): printed
    // here, after every child is reaped, from the records just collected
    // above plus this driver's OWN pid[i]/g_kind[i] (never transmitted --
    // the driver already knows them). A task that never sent a record
    // (collection broke, or the task was never created) is reported
    // explicitly as missing, not silently skipped -- an aggregator seeing
    // fewer MIXBENCH_TASK lines than SCHEDULE entries already flags that
    // via its idx-coverage check.
    for(i = 0; i < NTASKS; i++){
      if(pid[i] < 0)
        continue;
      if(!task_present[i]){
        fprintf(ERRFD, "mixbench: idx=%d has no collected result record "
                "(collect_broken=%d)\n", i, collect_broken);
        continue;
      }
      printf("MIXBENCH_TASK idx=%d pid=%d kind=%d t_resume=%lu t_complete=%lu "
             "stats_ok=%d source=self_at_exit collect_mode=%s run_ticks=%lu "
             "wait_total=%lu wait_max=%lu open_wait=%lu task_failed=%d\n",
             i, pid[i], g_kind[i], task_t_resume[i], task_t_complete[i],
             task_ok[i], MIXBENCH_OUTPUT_MODE, task_run_ticks[i],
             task_wait_total[i], task_wait_max[i], task_open_wait[i],
             task_failed_flag[i]);
    }
#endif

    // Now that every child has been reaped, no other process is left to
    // interleave with the driver's own console output -- print the
    // deferred MIXBENCH_CKPT lines here, using the values captured (at the
    // original, unchanged timing) right after the release loop.
    for(i = 0; i < NTASKS; i++){
      if(!ckpt_present[i])
        continue;
      if(ckpt_ok[i])
        printf("MIXBENCH_CKPT idx=%d pid=%lu ck=a ck_tick=%lu query_tick=%lu "
               "source=live_query run_ticks=%lu wait_total=%lu wait_max=%lu "
               "open_wait=%lu unavailable=0\n",
               i, ckpt_pid[i], ckpt_ck_a[i], ckpt_query_tick[i],
               ckpt_out[i][0], ckpt_out[i][1], ckpt_out[i][2], ckpt_out[i][3]);
      else
        printf("MIXBENCH_CKPT idx=%d pid=%lu ck=a ck_tick=%lu query_tick=%lu "
               "source=live_query unavailable=1\n",
               i, ckpt_pid[i], ckpt_ck_a[i], ckpt_query_tick[i]);
    }

    printf("MIXBENCH_CKPT_B tick=%lu\n", t_end);

    int all_expected_reaped = (nreaped == ncreated);
    int all_exit_ok = 1;
    if(all_expected_reaped){
      for(i = 0; i < NTASKS; i++){
        if(pid[i] < 0)
          continue;
        int found = 0;
        for(k = 0; k < nreaped; k++){
          if(reaped_pid[k] == pid[i]){
            found = 1;
            if(reaped_status[k] != 0)
              all_exit_ok = 0;
            break;
          }
        }
        if(!found)
          all_expected_reaped = 0;
      }
    }

    int run_valid = all_expected_reaped && all_exit_ok &&
                    !any_release_failed && (ncreated == NTASKS);
#if MIXBENCH_QUIET_OUTPUT
    // A results-collection failure (broken pipe read, or fewer records than
    // created tasks) must sink run_valid too -- otherwise a quiet-mode run
    // that silently lost some MIXBENCH_TASK data could still report
    // run_valid=1, which is exactly the "partial data treated as PASS"
    // failure mode this project's rigor elsewhere explicitly rejects.
    if(collect_broken || ncollected != ncreated)
      run_valid = 0;
#endif

    // SCHED-EP2-BUDGET-01 (2026-09-14 review): the end snapshot is taken
    // HERE -- before run_valid is finalized and before MIXBENCH_DONE is
    // printed -- specifically so a failed query (either end, or the start
    // query above) sinks run_valid the same way a collection failure does,
    // instead of the stats line just quietly not appearing. A caller must
    // never be able to mistake "the syscall failed" for "the policy
    // reported zero engagement".
    uint64 budget_end[3];
    int budget_end_ok = (sched_budget_stats(budget_end) == 0);
    int budget_stats_ok = budget_start_ok && budget_end_ok;
    if(!budget_stats_ok)
      run_valid = 0;

    // SCHED-EP2-BUDGET-01 (2026-09-14 review): printed BEFORE MIXBENCH_DONE
    // on purpose -- MIXBENCH_DONE is the sentinel line an external
    // collector watches for and may stop reading right after seeing it, so
    // a stats line printed AFTER it can be silently missed. Printing this
    // first means a collector that stops at MIXBENCH_DONE has already seen
    // it. Always printed, even on query failure (explicit stats_ok=0
    // marker), never just omitted. Values are a before/after DELTA across
    // this workload's window (t0..t_end), not a raw cumulative-since-boot
    // read -- see the start-snapshot comment above. All-zero under any
    // policy that doesn't track a budget (design spec S4).
    if (budget_stats_ok) {
      printf("MIXBENCH_BUDGET_STATS stats_ok=1 ls_charged_ticks=%lu budget_exhaustions=%lu "
             "normal_selected_while_ls_runnable=%lu\n",
             budget_end[0] - budget_start[0],
             budget_end[1] - budget_start[1],
             budget_end[2] - budget_start[2]);
    } else {
      printf("MIXBENCH_BUDGET_STATS stats_ok=0 ls_charged_ticks=0 budget_exhaustions=0 "
             "normal_selected_while_ls_runnable=0\n");
    }

    // MIXBENCH_DONE is the one line an aggregator should treat as
    // authoritative for "did this run actually complete cleanly" --
    // partial creation (ncreated < NTASKS), a reap mismatch, any non-zero
    // child exit status (which now includes a task's own failed self
    // sched_stats query), a failed release write(), or a failed budget-stats
    // query (either snapshot) all fold into run_valid explicitly rather
    // than being silently treated as PASS (DECISIONS.md MW-R5-01
    // instruction 4).
    // SCHED-EP2-BUDGET-01 S11: dumps the span-observation per-window LS
    // execution-time table (kernel/span_observe.cpp), if the kernel was
    // built with EP2_SPAN_OBSERVE_ENABLED=1 -- prints "DISABLED" otherwise
    // (the OFF half of the OFF/ON pilot pair). Printed before MIXBENCH_DONE
    // for the same reason as MIXBENCH_BUDGET_STATS above.
    sched_span_dump();

    printf("MIXBENCH_DONE condition=%s ncreated=%d nreaped=%d all_expected_reaped=%d "
           "all_exit_ok=%d any_release_failed=%d run_valid=%d t0=%lu t_end=%lu "
           "mode_label=%s\n",
           MIXBENCH_CONDITION, ncreated, nreaped, all_expected_reaped,
           all_exit_ok, any_release_failed, run_valid, t0, t_end,
           MIXBENCH_MODE_LABEL);

    // The driver's own exit status mirrors run_valid -- a caller (or a
    // shell script wrapping this in a loop across repeated runs) can check
    // it directly instead of having to re-parse MIXBENCH_DONE itself.
    exit(run_valid ? 0 : 1);
  }
}
