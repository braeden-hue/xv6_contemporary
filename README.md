# modern_xv6

Experimental xv6-riscv fork studying Modern C++ abstraction costs and scheduling-policy trade-offs in a freestanding kernel.

The project follows two connected questions:

1. What code and runtime cost does compile-time policy abstraction produce?
2. How do preemption rules affect latency, background progress, and CPU share?

The repository is based on MIT xv6-riscv. The original `mmap` work began as an operating-systems course assignment; the Modern C++ scheduler, measurement infrastructure, priority-preemption research, and related experiments are personal extensions.

## Current Status

| Area | Status | Current implementation |
|---|---|---|
| Scheduler architecture | ✅ | Compile-time policies with separate selection (`pick_next`) and preemption (`should_preempt`) interfaces |
| Round Robin / FCFS | ✅ | Stateful RR and arrival-sequence-based FCFS selection |
| Dispatch benchmark | ✅ | Static-template vs function-pointer dispatch measured, with disassembly analysis |
| Priority-based preemption | ✅ | `PriorityPreempt<UseCounter>` + `setpriority`; initial workload comparison measured (pending independent reproduction, see below) |
| Scheduler accounting | ✅ | `sched_stats` exposes per-process run ticks and runnable-wait accounting; automated regression test passing |
| RISC-V counters | ✅ | `rdcycle` / `rdinstret` integrated into the benchmark infrastructure |
| CFS | Deferred | Design only; intentionally deferred while the priority-preemption experiments take precedence (see `plan.md`) |
| mmap C++ port | ✅ | Behavior-preserving port used to study stronger type checking |

## Scheduler Architecture

The original xv6 scheduler keeps process selection and dispatch mechanics in the same control path. This fork separates two independent policy decisions:

- **Selection** (`pick_next(procs, n)`): which RUNNABLE process runs next.
- **Preemption** (`should_preempt(p)`): whether the currently running process should yield.

Shared dispatch code owns locking, the `RUNNABLE -> RUNNING` transition, and the context switch; a policy only answers those two questions.

```cpp
template<typename P>
concept SchedulerPolicy = requires(P policy, struct proc* procs, int n, struct proc* running) {
    { policy.pick_next(procs, n) } -> std::same_as<struct proc*>;
    { policy.should_preempt(running) } -> std::same_as<bool>;
};
```

```cpp
template<SchedulerPolicy P>
[[noreturn]] static void dispatch(P& policy)
{
    ...
    struct proc* next = policy.pick_next(proc, NPROC);
    ...
}

using ActivePolicy = PriorityPreempt<true>;
```

Policy selection stays at compile time rather than introducing a virtual interface into the scheduler hot path — `nm` on the built kernel shows no symbol for a policy that isn't the active one; an unused policy's code doesn't exist in the binary. RR, FCFS, and `PriorityPreempt<UseCounter>` are implemented as separate policy types; CFS is designed but deferred (see Current Status).

The kernel remains freestanding: no exceptions, RTTI, or hosted C++ runtime is assumed.

## Experiment 1: Dispatch Abstraction Cost

Compares compile-time template dispatch against runtime function-pointer dispatch for the same scheduling logic, under `CPUS=1` and `-icount shift=0` (deterministic instruction counting; see `docs/bench/`).

Under the recorded benchmark configuration, function-pointer dispatch retired 12 more instructions per call for a trivial O(1) body, and 15 more for the RR scan body once the process count was made opaque (`volatile`) to the compiler.

An initial RR-body measurement showed the *static* version retiring 174 more instructions than the function-pointer version — the opposite of what the call-path numbers predicted. Disassembly traced this to GCC's strength-reduction of `% 64` into a software instruction sequence once inlining exposed the compile-time-constant process count (`NPROC=64`); the function-pointer version, unable to see that constant, emitted a single hardware `remw` instead and came out ahead by coincidence. Making the process count `volatile` (opaque to the compiler) removed the reversal, isolating the call-path comparison from that unrelated codegen effect.

These results characterize the measured code paths and this exact compiler/optimization configuration; they are not a general claim that templates are always faster. Raw logs, objdump output, and reproduction commands: [`docs/bench/`](docs/bench/).

## Experiment 2: Priority-Based Preemption

With RR selection held as the baseline, `PriorityPreempt<UseCounter>` (`kernel/sched_priority.hpp`) changes only the preemption rule: a process declares itself latency-sensitive via the `setpriority` syscall (never inferred from run length), and a latency-sensitive process is never preempted by this policy while it holds the CPU.

Initial reported measurements (`user/latencytest.c`): a latency-sensitive task B's p99 response time dropped from 19 to 8 ticks (target-miss rate 5%→0%), while four background CPU-bound tasks' completion time increased by roughly 2.3×. **These latency numbers are self-reported and not yet independently reproduced or archived as raw logs** — treat them as a preliminary result, not a validated benchmark.

The follow-up work adds `sched_stats` (a new syscall) to measure each process's cumulative CPU ticks and RUNNABLE-wait ticks directly, including an in-progress wait at query time — making the A-task's progress (or lack of it) directly observable instead of inferred. This part *is* validated: `usertests -q` passes with the added accounting, and an automated test (`user/statstest.c`) confirms both the closed-interval accounting (exact match to the expected round-robin math) and the open-interval branch (a clean, predicted staircase across four competing processes). Raw logs: [`docs/bench/phase18b_usertests_raw_output.txt`](docs/bench/phase18b_usertests_raw_output.txt), [`docs/bench/phase18b_statstest_raw_output.txt`](docs/bench/phase18b_statstest_raw_output.txt).

Accounting is tick-granularity only (a sub-tick run or wait segment can be missed or double-counted at a boundary) — this is a known, accepted limitation, not a hidden one.

## Measurement and Validation

- `usertests -q`: **PASS ALL TESTS** on every scheduler-affecting change (FCFS, RR, priority preemption, and the accounting instrumentation). Raw logs in `docs/bench/`.
- `statstest`: automated regression test for the `sched_stats` accounting, no human-timing dependency. Raw log: `docs/bench/phase18b_statstest_raw_output.txt`.
- Measurement conditions (compiler, `CPUS`, QEMU flags, `-icount`) are recorded alongside each raw log rather than asserted from memory.

Development uses Claude Code and OpenAI Codex for implementation and independent code review. Across the scheduler-accounting work, five review rounds identified and corrected concrete defects — lock ordering/atomicity, 32-bit tick-counter wraparound, a query-time snapshot-ordering bug, and a missed final tick before process termination — before the work was accepted; the raw evidence behind that review is linked above.

## mmap Type-Safety Experiment

`sys_mmap` was ported from C to C++ without intentionally changing its behavior. This experiment is separate from the scheduler work: its purpose is to examine where stronger compile-time checks are useful in low-level code.

The port replaces selected C idioms with:

```text
PGROUNDUP macro            -> constexpr function
indexed VMA iteration      -> range-based for
field-by-field assignment  -> designated initialization
```

This part of the project is about **type/structure safety**, not a performance claim.

## Toolchain and Build

The Makefile is configured for a GCC 14 RISC-V cross toolchain and C++23 kernel sources. C++ files are compiled with freestanding constraints including `-ffreestanding`, `-fno-exceptions`, and `-fno-rtti`.

Requirements:

- RISC-V GCC/G++ 14 cross toolchain
- QEMU >= 7.2

Run xv6:

```bash
make qemu
```

Run the scheduler-accounting regression test inside xv6:

```
$ usertests -q
$ statstest
```

Run GCC static analysis for the C++ kernel files:

```bash
make analyze
```

The Makefile also emits disassembly and symbol files for the kernel, used for the dispatch experiment above.

## Next Steps

- Independently reproduce and archive the priority-preemption latency results (Experiment 2) as raw logs.
- Measure the linear-scan `PriorityPreempt<false>` baseline against the counter-based variant to complete the decision-cost comparison.
- Reproduce a minimal starvation scenario for latency-sensitive processes and evaluate a bounded execution budget as a fix.
- Use the CPU-share / runnable-wait measurements from `sched_stats` to evaluate future budget/aging policy changes.
- Revisit CFS and a broader selection × preemption comparison matrix after the above.

## Base Project and License

This repository is based on [MIT xv6-riscv](https://github.com/mit-pdos/xv6-riscv), a teaching operating system used in MIT 6.1810.

The original xv6 copyright and license are preserved in [`LICENSE`](LICENSE).
