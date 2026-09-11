#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

struct spinlock tickslock;
uint ticks;

extern char trampoline[], uservec[];

// in kernelvec.S, calls kerneltrap().
void kernelvec();

extern int devintr();
extern int policy_should_preempt(struct proc *p);   // kernel/scheduler.cpp (Phase 1.8)

void
trapinit(void)
{
  initlock(&tickslock, "time");
}

// set up to take exceptions and traps while in the kernel.
void
trapinithart(void)
{
  w_stvec((uint64)kernelvec);
}

//
// handle an interrupt, exception, or system call from user space.
// called from, and returns to, trampoline.S
// return value is user satp for trampoline.S to switch to.
//
uint64
usertrap(void)
{
  int which_dev = 0;

  if((r_sstatus() & SSTATUS_SPP) != 0)
    panic("usertrap: not from user mode");

  // send interrupts and exceptions to kerneltrap(),
  // since we're now in the kernel.
  w_stvec((uint64)kernelvec);  //DOC: kernelvec

  struct proc *p = myproc();

  // save user program counter.
  p->trapframe->epc = r_sepc();

  if(r_scause() == 8){
    // system call

    if(killed(p))
      kexit(-1);

    // sepc points to the ecall instruction,
    // but we want to return to the next instruction.
    p->trapframe->epc += 4;

    // an interrupt will change sepc, scause, and sstatus,
    // so enable only now that we're done with those registers.
    intr_on();

    syscall();
  } else if((which_dev = devintr()) != 0){
    // ok
  } else if(r_scause() == 13 || r_scause() == 15){
    // page fault: try lazy sbrk fill-in first, then lazy mmap fill-in.
    // The two paths are disjoint: vmfault() only handles va < p->sz,
    // mmapfault() only handles addresses inside an mmap VMA.
    uint64 stval = r_stval();
    if(vmfault(p->pagetable, stval, (r_scause() == 13) ? 1 : 0) != 0){
      // lazily-allocated heap page handled
    } else if(mmapfault(stval) == 0){
      // mmap lazy fault handled ok.
    } else {
      printf("usertrap(): unexpected scause 0x%lx pid=%d\n", r_scause(), p->pid);
      printf("            sepc=0x%lx stval=0x%lx\n", r_sepc(), r_stval());
      setkilled(p);
    }
  } else {
    printf("usertrap(): unexpected scause 0x%lx pid=%d\n", r_scause(), p->pid);
    printf("            sepc=0x%lx stval=0x%lx\n", r_sepc(), r_stval());
    setkilled(p);
  }

  // Phase 1.8b: charge this tick to whoever was actually running when it
  // fired -- BEFORE the killed(p)/kexit(-1) check right below, because
  // kexit() is noreturn: a proc killed by someone else just before this
  // same trap must still get credit for the tick it just consumed, or
  // its very last tick of execution silently vanishes from
  // run_ticks_total (Codex Implementation Gate audit, 2026-09-12).
  // Deliberately NOT under p->lock: the only writer of a given proc's
  // run_ticks_total is that proc's own trap path on the one CPU it's
  // actually running on (never concurrent with itself), so this
  // increment itself can't race with another increment. sys_sched_stats()
  // (kernel/sysproc.c) reads this field from a different CPU while
  // holding p->lock, which does NOT synchronize with this write -- so
  // both sides use __atomic_fetch_add/__atomic_load_n (relaxed) instead
  // of a plain `++`/read, per the same audit: RV64's aligned-access
  // non-tearing guarantee is a hardware property, not a substitute for
  // defined C/C++ concurrent-access semantics.
  if(which_dev == 2)
    __atomic_fetch_add(&p->run_ticks_total, 1, __ATOMIC_RELAXED);

  if(killed(p))
    kexit(-1);

  // give up the CPU if this is a timer interrupt and the active policy
  // says so (Phase 1.8). RR/FCFS's should_preempt() always returns true,
  // so this is behaviorally identical to the old unconditional yield().
  if(which_dev == 2 && policy_should_preempt(p))
    yield();

  prepare_return();

  // the user page table to switch to, for trampoline.S
  uint64 satp = MAKE_SATP(p->pagetable);

  // return to trampoline.S; satp value in a0.
  return satp;
}

//
// set up trapframe and control registers for a return to user space
//
void
prepare_return(void)
{
  struct proc *p = myproc();

  // we're about to switch the destination of traps from
  // kerneltrap() to usertrap(). because a trap from kernel
  // code to usertrap would be a disaster, turn off interrupts.
  intr_off();

  // send syscalls, interrupts, and exceptions to uservec in trampoline.S
  uint64 trampoline_uservec = TRAMPOLINE + (uservec - trampoline);
  w_stvec(trampoline_uservec);

  // set up trapframe values that uservec will need when
  // the process next traps into the kernel.
  p->trapframe->kernel_satp = r_satp();         // kernel page table
  p->trapframe->kernel_sp = p->kstack + PGSIZE; // process's kernel stack
  p->trapframe->kernel_trap = (uint64)usertrap;
  p->trapframe->kernel_hartid = r_tp();         // hartid for cpuid()

  // set up the registers that trampoline.S's sret will use
  // to get to user space.

  // set S Previous Privilege mode to User.
  unsigned long x = r_sstatus();
  x &= ~SSTATUS_SPP; // clear SPP to 0 for user mode
  x |= SSTATUS_SPIE; // enable interrupts in user mode
  w_sstatus(x);

  // set S Exception Program Counter to the saved user pc.
  w_sepc(p->trapframe->epc);
}

// interrupts and exceptions from kernel code go here via kernelvec,
// on whatever the current kernel stack is.
void
kerneltrap()
{
  int which_dev = 0;
  uint64 sepc = r_sepc();
  uint64 sstatus = r_sstatus();
  uint64 scause = r_scause();

  if((sstatus & SSTATUS_SPP) == 0)
    panic("kerneltrap: not from supervisor mode");
  if(intr_get() != 0)
    panic("kerneltrap: interrupts enabled");

  if((which_dev = devintr()) == 0){
    // interrupt or trap from an unknown source
    printf("scause=0x%lx sepc=0x%lx stval=0x%lx\n", scause, r_sepc(), r_stval());
    panic("kerneltrap");
  }

  // give up the CPU if this is a timer interrupt.
  if(which_dev == 2 && myproc() != 0){
    // Phase 1.8b: same tick-charging as usertrap() above (atomic, same
    // reasoning -- see that comment), kept in sync so a proc that's
    // mid-syscall (running kernel code) when the timer fires still gets
    // its run_ticks_total credited -- otherwise kernel-heavy workloads
    // would look artificially starved in the fairness stats relative to
    // user-mode-heavy ones.
    __atomic_fetch_add(&myproc()->run_ticks_total, 1, __ATOMIC_RELAXED);
    yield();
  }

  // the yield() may have caused some traps to occur,
  // so restore trap registers for use by kernelvec.S's sepc instruction.
  w_sepc(sepc);
  w_sstatus(sstatus);
}

void
clockintr()
{
  if(cpuid() == 0){
    acquire(&tickslock);
    // Phase 1.8b: atomic increment (not `ticks++`) so the Phase 1.8b
    // instrumentation's lock-free readers (kernel/proc.c's mark_runnable(),
    // kernel/scheduler.cpp's dispatch(), kernel/sysproc.c's
    // sys_sched_stats() -- see their comments for why they can't just take
    // tickslock, to avoid a tickslock-inside-p->lock ordering that
    // conflicts with wakeup() below acquiring p->lock while tickslock is
    // still held) have a well-defined value to load instead of racing with
    // this plain read-modify-write. tickslock itself is still held, for
    // the existing wakeup() ordering -- this is about the *readers*
    // outside this lock, not about this writer needing more protection.
    // __atomic_* (not this file's/kernel/proc.c's existing
    // __sync_fetch_and_add/__sync_fetch_and_sub for g_ls_runnable_count):
    // both are freestanding-safe GCC/Clang compiler builtins that lower
    // directly to RISC-V AMO instructions with zero library symbols (same
    // category already proven in this exact build, see plan.md's
    // freestanding-availability table for std::atomic) -- __atomic_* is
    // used here specifically because it has a genuine relaxed *load*
    // (__atomic_load_n), which __sync_* has no equivalent for without
    // faking one as a wasteful RMW (e.g. fetch_and_add(&x, 0)).
    __atomic_fetch_add(&ticks, 1, __ATOMIC_RELAXED);
    wakeup(&ticks);
    release(&tickslock);
  }

  // ask for the next timer interrupt. this also clears
  // the interrupt request. 1000000 is about a tenth
  // of a second.
  w_stimecmp(r_time() + 1000000);
}

// check if it's an external interrupt or software interrupt,
// and handle it.
// returns 2 if timer interrupt,
// 1 if other device,
// 0 if not recognized.
int
devintr()
{
  uint64 scause = r_scause();

  if(scause == 0x8000000000000009L){
    // this is a supervisor external interrupt, via PLIC.

    // irq indicates which device interrupted.
    int irq = plic_claim();

    if(irq == UART0_IRQ){
      uartintr();
    } else if(irq == VIRTIO0_IRQ){
      virtio_disk_intr();
    } else if(irq){
      printf("unexpected interrupt irq=%d\n", irq);
    }

    // the PLIC allows each device to raise at most one
    // interrupt at a time; tell the PLIC the device is
    // now allowed to interrupt again.
    if(irq)
      plic_complete(irq);

    return 1;
  } else if(scause == 0x8000000000000005L){
    // timer interrupt.
    clockintr();
    return 2;
  } else {
    return 0;
  }
}

