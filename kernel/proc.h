#pragma once   // needed once C++ files started composing multiple headers that
               // each pull this one in (Phase 1); the original .c files never
               // double-included it so this was never needed before.

// Saved registers for kernel context switches.
struct context {
  uint64 ra;
  uint64 sp;

  // callee-saved
  uint64 s0;
  uint64 s1;
  uint64 s2;
  uint64 s3;
  uint64 s4;
  uint64 s5;
  uint64 s6;
  uint64 s7;
  uint64 s8;
  uint64 s9;
  uint64 s10;
  uint64 s11;
};

// Per-CPU state.
struct cpu {
  struct proc *proc;          // The process running on this cpu, or null.
  struct context context;     // swtch() here to enter scheduler().
  int noff;                   // Depth of push_off() nesting.
  int intena;                 // Were interrupts enabled before push_off()?
};

extern struct cpu cpus[NCPU];

// per-process data for the trap handling code in trampoline.S.
// sits in a page by itself just under the trampoline page in the
// user page table. not specially mapped in the kernel page table.
// uservec in trampoline.S saves user registers in the trapframe,
// then initializes registers from the trapframe's
// kernel_sp, kernel_hartid, kernel_satp, and jumps to kernel_trap.
// usertrapret() and userret in trampoline.S set up
// the trapframe's kernel_*, restore user registers from the
// trapframe, switch to the user page table, and enter user space.
// the trapframe includes callee-saved user registers like s0-s11 because the
// return-to-user path via usertrapret() doesn't return through
// the entire kernel call stack.
struct trapframe {
  /*   0 */ uint64 kernel_satp;   // kernel page table
  /*   8 */ uint64 kernel_sp;     // top of process's kernel stack
  /*  16 */ uint64 kernel_trap;   // usertrap()
  /*  24 */ uint64 epc;           // saved user program counter
  /*  32 */ uint64 kernel_hartid; // saved kernel tp
  /*  40 */ uint64 ra;
  /*  48 */ uint64 sp;
  /*  56 */ uint64 gp;
  /*  64 */ uint64 tp;
  /*  72 */ uint64 t0;
  /*  80 */ uint64 t1;
  /*  88 */ uint64 t2;
  /*  96 */ uint64 s0;
  /* 104 */ uint64 s1;
  /* 112 */ uint64 a0;
  /* 120 */ uint64 a1;
  /* 128 */ uint64 a2;
  /* 136 */ uint64 a3;
  /* 144 */ uint64 a4;
  /* 152 */ uint64 a5;
  /* 160 */ uint64 a6;
  /* 168 */ uint64 a7;
  /* 176 */ uint64 s2;
  /* 184 */ uint64 s3;
  /* 192 */ uint64 s4;
  /* 200 */ uint64 s5;
  /* 208 */ uint64 s6;
  /* 216 */ uint64 s7;
  /* 224 */ uint64 s8;
  /* 232 */ uint64 s9;
  /* 240 */ uint64 s10;
  /* 248 */ uint64 s11;
  /* 256 */ uint64 t3;
  /* 264 */ uint64 t4;
  /* 272 */ uint64 t5;
  /* 280 */ uint64 t6;
};

enum procstate { UNUSED, USED, SLEEPING, RUNNABLE, RUNNING, ZOMBIE };

// mmap virtual memory area. One slot per active mapping.
#define NVMA 16

struct vma {
  int          used;     // 1 if this slot is in use
  uint64       addr;     // start virtual address (page-aligned)
  uint64       length;   // length in bytes (PGSIZE multiple)
  int          prot;     // PROT_READ / PROT_WRITE
  int          flags;    // MAP_SHARED / MAP_PRIVATE
  int          offset;   // file offset (always 0 in this lab)
  struct file *f;        // mapped file (refcounted via filedup)
};

// Per-process state
struct proc {
  struct spinlock lock;

  // p->lock must be held when using these:
  enum procstate state;        // Process state
  void *chan;                  // If non-zero, sleeping on chan
  int killed;                  // If non-zero, have been killed
  int xstate;                  // Exit status to be returned to parent's wait
  int pid;                     // Process ID

  // wait_lock must be held when using this:
  struct proc *parent;         // Parent process

  // these are private to the process, so p->lock need not be held.
  uint64 kstack;               // Virtual address of kernel stack
  uint64 sz;                   // Size of process memory (bytes)
  pagetable_t pagetable;       // User page table
  struct trapframe *trapframe; // data page for trampoline.S
  struct context context;      // swtch() here to run process
  struct file *ofile[NOFILE];  // Open files
  struct inode *cwd;           // Current directory
  char name[16];               // Process name (debugging)
  struct vma vmas[NVMA];       // mmap regions
  uint64 arrival_seq;          // Phase 1 (FCFS): order this proc last became RUNNABLE
  int priority;                // Phase 1.8: 0=Normal, 1=LatencySensitive. Explicit only
                                // (set via syscall) -- never inferred from run length.

  // Phase 1.8b: per-task CPU share / max runnable-wait instrumentation
  // (plan.md/HANDOFF.md "다음 작업 순서" item 3). Tick-granularity only --
  // inherits the same approximation limits already documented for this
  // project's tick-based accounting (a sub-tick run/wait segment can be
  // missed or double-counted at a tick boundary). Reset to 0 by
  // allocproc() on slot reuse (see the comment there) so a new process
  // never inherits a previous occupant's accumulated ticks.
  //
  // Locking is NOT uniform across these four fields (Codex Implementation
  // Gate audit, 2026-09-12 -- an earlier draft of this comment claimed
  // "p->lock must be held for all of these", which was wrong for the last
  // one):
  //   - sched_ready_tick, wait_ticks_total, wait_ticks_max: written only
  //     while p->lock is held (mark_runnable() in kernel/proc.c opens the
  //     interval, dispatch() in kernel/scheduler.cpp closes it), and read
  //     only while p->lock is held (sys_sched_stats() in kernel/sysproc.c)
  //     -- ordinary p->lock-protected fields, no atomics needed.
  //   - run_ticks_total: incremented from kernel/trap.c's usertrap()/
  //     kerneltrap() WITHOUT p->lock (that path never takes it), so every
  //     increment/read after the proc is live goes through the atomic
  //     counter ops (__atomic_fetch_add()/__atomic_load_n(), relaxed),
  //     never a plain `++`/read, regardless of whether the caller happens
  //     to also hold p->lock. The one exception is allocproc()'s plain
  //     `p->run_ticks_total = 0;` on slot reuse (kernel/proc.c) -- that's
  //     not a race because it runs before the proc can be observed by any
  //     other CPU (p->lock is held and the proc is still UNUSED/USED, not
  //     yet runnable), same reasoning as p->priority's reset right above it.
  uint64 sched_ready_tick;     // tick at which this proc most recently became RUNNABLE
  uint64 wait_ticks_total;     // cumulative RUNNABLE-wait ticks, closed intervals only
                                // (i.e. up to the last time it was actually dispatched --
                                // a still-waiting proc's *open* interval is NOT included
                                // here; sys_sched_stats() adds that back in at read time
                                // so a mid-experiment sample doesn't make starvation
                                // invisible just because the wait hasn't ended yet)
  uint64 wait_ticks_max;       // longest single closed RUNNABLE-wait interval observed
  uint64 run_ticks_total;      // cumulative ticks this proc was the one actually
                                // RUNNING, charged once per timer tick from both
                                // usertrap() and kerneltrap() (kernel/trap.c) --
                                // atomic, see locking note above
};

extern struct proc proc[NPROC];   // Phase 1: sched_fcfs.cpp's pick_next() scans this
                                   // (must come after struct proc is complete)
