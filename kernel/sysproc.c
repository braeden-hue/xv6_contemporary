#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"
#include "vm.h"

uint64
sys_exit(void)
{
  int n;
  argint(0, &n);
  kexit(n);
  return 0;  // not reached
}

uint64
sys_getpid(void)
{
  return myproc()->pid;
}

uint64
sys_fork(void)
{
  return kfork();
}

uint64
sys_wait(void)
{
  uint64 p;
  argaddr(0, &p);
  return kwait(p);
}

uint64
sys_sbrk(void)
{
  uint64 addr;
  int t;
  int n;

  argint(0, &n);
  argint(1, &t);
  addr = myproc()->sz;

  if(t == SBRK_EAGER || n < 0) {
    if(growproc(n) < 0) {
      return -1;
    }
  } else {
    // Lazily allocate memory for this process: increase its memory
    // size but don't allocate memory. If the processes uses the
    // memory, vmfault() will allocate it.
    if(addr + n < addr)
      return -1;
    if(addr + n > TRAPFRAME)
      return -1;
    myproc()->sz += n;
  }
  return addr;
}

uint64
sys_pause(void)
{
  int n;
  uint ticks0;

  argint(0, &n);
  if(n < 0)
    n = 0;
  acquire(&tickslock);
  ticks0 = ticks;
  while(ticks - ticks0 < n){
    if(killed(myproc())){
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  return 0;
}

uint64
sys_kill(void)
{
  int pid;

  argint(0, &pid);
  return kkill(pid);
}

// Phase 1.8: a process explicitly declares its own priority. Never called
// on another pid's behalf, never inferred from run length -- length and
// urgency are separate information (see plan.md Phase 1.8).
#include "span_observe_config.h"
#if EP2_SPAN_OBSERVE_ENABLED
// old_priority is the value BEFORE this call -- span_observe.cpp needs it
// to tell a real transition apart from a same-value call (e.g. a LONG
// task's Normal->Normal setpriority(0) under QD), which must be a no-op
// for the span accounting, not misread as a priority flip (2026-09-14
// 3rd review, bug 1).
extern void span_observe_priority_change(struct proc*, int old_priority);
#endif

uint64
sys_setpriority(void)
{
  int level;
#if EP2_SPAN_OBSERVE_ENABLED
  int old_priority = myproc()->priority;
#endif

  argint(0, &level);
  if (level != 0 && level != 1)
    return -1;
  myproc()->priority = level;
#if EP2_SPAN_OBSERVE_ENABLED
  span_observe_priority_change(myproc(), old_priority);
#endif
  return 0;
}

// SCHED-EP2-BUDGET-01 S11: triggers the kernel to printf its per-window LS
// execution-time table (kernel/span_observe.cpp). No arguments, no
// copyout -- this is a scoped, temporary diagnostic tool, not a
// permanent public ABI addition like sched_stats/sched_budget_stats.
extern void span_observe_dump(void);

uint64
sys_sched_span_dump(void)
{
  span_observe_dump();
  return 0;
}

// Phase 1.8b: read back one pid's tick-based CPU/wait accounting for
// fairness measurement (plan.md/HANDOFF.md "다음 작업 순서" item 3). Scans
// proc[] by pid, same convention as kkill()/kwait() in kernel/proc.c --
// no new locking primitive introduced. Writes 4 uint64s to *addr:
//   [0] run_ticks_total   -- cumulative ticks actually RUNNING
//   [1] wait_ticks_total  -- cumulative RUNNABLE-wait, closed intervals only
//   [2] wait_ticks_max    -- longest single closed RUNNABLE-wait
//   [3] open_wait_ticks   -- 0 unless the proc is RUNNABLE right now; then
//                            it's ticks-sched_ready_tick, so a still-waiting
//                            proc isn't invisible to a caller sampling
//                            mid-experiment (see proc.h's field comment)
// A caller wanting a single "worst-case wait so far" number should compute
// max(out[2], out[3]) itself -- out[2] alone misses an in-progress wait
// that hasn't been closed (and therefore hasn't had a chance to become the
// max) yet (Codex Implementation Gate audit, 2026-09-12).
// Returns -1 if pid isn't a live (non-UNUSED) proc, or the copyout fails.
uint64
sys_sched_stats(void)
{
  int pid;
  uint64 addr;
  struct proc *p, *caller = myproc();
  uint64 out[4];
  int found = 0;

  argint(0, &pid);
  argaddr(1, &addr);

  for(p = proc; p < &proc[NPROC]; p++){
    acquire(&p->lock);
    if(p->pid == pid && p->state != UNUSED){
      // Phase 1.8b, Codex Implementation Gate audit (2026-09-12): read
      // `ticks` AFTER acquiring p->lock, not before the scan -- p's state
      // and sched_ready_tick are only consistent with each other at this
      // instant. Reading the clock earlier (e.g. before the loop) lets an
      // arbitrary scheduling delay land between that read and actually
      // observing p's state, which can shrink or zero out open_wait_ticks
      // for a proc that's really been waiting the whole time. run_ticks_total
      // is read via __atomic_load_n since trap.c's writer doesn't hold
      // p->lock (see that comment); wait_ticks_total/_max ARE p->lock-
      // protected on both the write side (scheduler.cpp's dispatch(), same
      // lock) and this read, so a plain read of those two is fine.
      uint32 now32 = (uint32)__atomic_load_n(&ticks, __ATOMIC_RELAXED);
      out[0] = __atomic_load_n(&p->run_ticks_total, __ATOMIC_RELAXED);
      out[1] = p->wait_ticks_total;
      out[2] = p->wait_ticks_max;
      out[3] = (p->state == RUNNABLE) ?
                 (uint64)(now32 - (uint32)p->sched_ready_tick) : 0;
      found = 1;
      release(&p->lock);
      break;
    }
    release(&p->lock);
  }
  if(!found)
    return -1;
  if(copyout(caller->pagetable, addr, (char *)out, sizeof(out)) < 0)
    return -1;
  return 0;
}

// SCHED-EP2-BUDGET-01 (design/ep2_selectonly_budget/spec.md S4): policy-level
// (not per-pid) diagnostics. Writes 3 uint64s to *addr:
//   [0] ls_charged_ticks               -- cumulative timer ticks charged to
//                                         an LS-priority proc (diagnostic,
//                                         never resets across windows)
//   [1] budget_exhaustions             -- count of window-transitions into
//                                         "exhausted" (at most 1 per window)
//   [2] normal_selected_while_ls_runnable -- count of ticks where the budget
//                                         was exhausted, an LS proc WAS
//                                         runnable, and a Normal proc got
//                                         picked anyway (the actual
//                                         "displacement prevented" signal)
// Policies without get_budget_stats() (RR/FCFS/PriorityPreempt/
// PrioritySelectOnly) report all-zero via policy_budget_stats()'s
// concept-gated else-branch (kernel/scheduler.cpp) -- this syscall itself
// is identical for every policy.
extern void policy_budget_stats(uint64 *out);

uint64
sys_sched_budget_stats(void)
{
  uint64 addr;
  struct proc *caller = myproc();
  uint64 out[3];

  argaddr(0, &addr);
  policy_budget_stats(out);
  if(copyout(caller->pagetable, addr, (char *)out, sizeof(out)) < 0)
    return -1;
  return 0;
}

// return how many clock tick interrupts have occurred
// since start.
uint64
sys_uptime(void)
{
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}
