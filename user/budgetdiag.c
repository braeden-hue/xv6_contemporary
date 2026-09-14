#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

// SCHED-EP2-BUDGET-01 positive-control functional diagnostic
// (design/ep2_selectonly_budget/spec.md rev2, 2026-09-14 review point 4-2).
//
// NOT part of the mixbench workload or its recorded results -- a standalone
// functional check that the Budget mechanism (charge -> exhaust -> a
// Normal proc gets selected while an LS proc is still waiting -> window
// reset) actually happens end-to-end in the real kernel, under whichever
// PrioritySelectBudget<...> instantiation is currently built in as
// ActivePolicy. Requires CPUS=1 (single core, enforced by
// docs/bench/stage_budget/run_budget_pilot.sh) so the two children
// genuinely compete for the one CPU instead of running in parallel on
// separate harts.
//
// Two children, unlike mixbench's SHORT tasks (which the 2026-09-13 pilot
// showed finish inside a single timer tick and so were never actually
// caught executing by policy_charge_tick()):
//   - LS: setpriority(1), then busy-loops long enough to span MANY timer
//     ticks -- calibrated the opposite way from mixbench on purpose.
//   - NORMAL: stays priority 0, busy-loops the same shape, so it is
//     RUNNABLE for the LS child's entire run -- this is what makes
//     normal_selected_while_ls_runnable observable at all (mixbench's LONG
//     tasks exist but aren't necessarily RUNNABLE at the exact tick an LS
//     task's budget happens to exhaust).
//
// A nonzero result in all three counters is the functional confirmation
// the mixbench pilot's zero result could not provide on its own -- this
// diagnostic answers "does the mechanism work at all", not "what does it
// do to the mixbench workload's performance", and is never pooled with
// Stage B/C or mixbench results.
// 2026-09-14: 200,000,000 (the first value tried) turned out to charge only
// ~5 ticks total for the whole LS run -- enough to prove charge_tick()
// fires for a multi-tick task at all (it does), but too close to
// BudgetTicks=5 for the 5 charges to reliably land inside a single
// WindowTicks=10 window (they could split 2+3 across a window boundary and
// never trip the exhaustion transition, which is exactly what the first
// run showed: budget_exhaustions=0, normal_selected_while_ls_runnable=0).
// Raised 10x so LS's run comfortably spans multiple full windows -- this
// is calibrating what the DIAGNOSTIC's own workload needs to actually
// exercise the mechanism, not tuning BudgetTicks/WindowTicks (the
// hypothesis parameters, left untouched) to manufacture a result.
#define DIAG_ITERS 2000000000UL

int
main(void)
{
  uint64 start[3], end[3];
  int ls_pid, normal_pid;
  volatile unsigned long dummy = 0;
  unsigned long i;

  if (sched_budget_stats(start) < 0) {
    printf("BUDGETDIAG start query failed\n");
    exit(1);
  }

  ls_pid = fork();
  if (ls_pid < 0) {
    printf("BUDGETDIAG fork(LS) failed\n");
    exit(1);
  }
  if (ls_pid == 0) {
    if (setpriority(1) < 0) {
      printf("BUDGETDIAG LS setpriority failed\n");
      exit(1);
    }
    for (i = 0; i < DIAG_ITERS; i++)
      dummy += i;
    exit(0);
  }

  normal_pid = fork();
  if (normal_pid < 0) {
    printf("BUDGETDIAG fork(Normal) failed\n");
    exit(1);
  }
  if (normal_pid == 0) {
    for (i = 0; i < DIAG_ITERS; i++)
      dummy += i;
    exit(0);
  }

  wait(0);
  wait(0);

  if (sched_budget_stats(end) < 0) {
    printf("BUDGETDIAG end query failed\n");
    exit(1);
  }

  printf("BUDGETDIAG ls_charged_ticks=%lu budget_exhaustions=%lu "
         "normal_selected_while_ls_runnable=%lu\n",
         end[0] - start[0], end[1] - start[1], end[2] - start[2]);
  exit(0);
}
