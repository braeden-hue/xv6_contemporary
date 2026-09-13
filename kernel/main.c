#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "defs.h"

volatile static int started = 0;

extern void run_calibration(void);    // kernel/bench.cpp: instret calibration (done, kept for re-verification)
extern void run_dispatch_bench(void); // kernel/bench.cpp: static vs fnptr pick_next() comparison
extern void run_resource_experiment(void); // kernel/bench_resource_harness.cpp: EP1-RESOURCE-01 -- needs kalloc(), run after kinit()

// Set to 0 for regression runs (usertests -q etc.) so the production
// benchmark/correctness harness isn't itself running during that check, per
// docs/episode1_next_experiment_plan.md's "run existing relevant
// allocator/user regression checks with the production benchmark disabled".
#define EP1_RESOURCE_RUN_AT_BOOT 1

// start() jumps here in supervisor mode on all CPUs.
void
main()
{
  if(cpuid() == 0){
    consoleinit();
    printfinit();
    printf("\n");
    printf("xv6 kernel is booting\n");
    printf("\n");
    run_dispatch_bench();
    kinit();         // physical page allocator
#if EP1_RESOURCE_RUN_AT_BOOT
    run_resource_experiment(); // needs a working kalloc(); before paging is fine (kinit already ran)
#endif
    kvminit();       // create kernel page table
    kvminithart();   // turn on paging
    procinit();      // process table
    trapinit();      // trap vectors
    trapinithart();  // install kernel trap vector
    plicinit();      // set up interrupt controller
    plicinithart();  // ask PLIC for device interrupts
    binit();         // buffer cache
    iinit();         // inode table
    fileinit();      // file table
    virtio_disk_init(); // emulated hard disk
    userinit();      // first user process
    __sync_synchronize();
    started = 1;
  } else {
    while(started == 0)
      ;
    __sync_synchronize();
    printf("hart %d starting\n", cpuid());
    kvminithart();    // turn on paging
    trapinithart();   // install kernel trap vector
    plicinithart();   // ask PLIC for device interrupts
  }

  scheduler();
}
