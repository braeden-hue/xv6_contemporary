# modern_xv6

Experimental xv6-riscv fork for studying the cost of Modern C++ abstractions in a freestanding kernel environment.

This project asks a narrow systems question: **can compile-time abstractions improve kernel structure without adding observable runtime cost in a hot path?** The current focus is scheduler policy abstraction with C++20/23 concepts and templates, followed by quantitative comparison using RISC-V cycle/instruction counters and generated assembly.

The repository is based on MIT xv6-riscv. The original `mmap` work began as an operating-systems course assignment; the Modern C++ scheduler, measurement infrastructure, and related experiments are personal extensions.

## Current Status

| Area | Status | Current implementation |
|---|---|---|
| Scheduler abstraction | ✅ | `SchedulerPolicy` concept + templated shared dispatch path |
| Round Robin | ✅ | Stateful RR policy using the last selected process index |
| FCFS | ✅ | Selects the longest-waiting RUNNABLE process using `arrival_seq` |
| CFS | 🚧 | In progress |
| RISC-V counters | ✅ | `rdcycle` / `rdinstret` access enabled for upcoming measurements |
| mmap C++ port | ✅ | Behavior-preserving port used to study stronger type checking |
| Dispatch benchmark | 🚧 | Static-template vs runtime function-pointer comparison planned |

## Scheduler: Policy vs. Mechanism

The original xv6 scheduler keeps process selection and dispatch mechanics in the same control path. This fork separates them.

Scheduling policies expose a minimal interface constrained by a C++20 concept:

```cpp
template<typename P>
concept SchedulerPolicy = requires(P policy, struct proc* procs, int n) {
    { policy.pick_next(procs, n) } -> std::same_as<struct proc*>;
};
```

The common dispatch path owns locking, the `RUNNABLE -> RUNNING` transition, and the context switch. The selected policy only decides which process should run next.

```cpp
template<SchedulerPolicy P>
[[noreturn]] static void dispatch(P& policy)
{
    ...
    struct proc* next = policy.pick_next(proc, NPROC);
    ...
}

using ActivePolicy = RR;
```

This keeps policy selection at compile time rather than introducing a virtual interface into the scheduler path. RR and FCFS are implemented as separate policy types, while CFS is still under development.

## Why Modern C++ in xv6?

The goal is not to rewrite xv6 in C++, nor to assume that C++ is inherently faster than C. The project uses a small set of language features where they can express an OS design constraint more directly and then checks what the compiler actually produces.

Three current examples are:

- **Concepts + templates** for scheduler policy/mechanism separation.
- **`constexpr` + `enum class`** for typed CSR configuration in early machine-mode setup.
- **Brace initialization + range-based loops** in a behavior-preserving C++ port of `sys_mmap` to explore compile-time checking around kernel data structures.

The kernel remains freestanding: no exceptions, RTTI, or hosted C++ runtime is assumed.

## Measurement Plan

The next experiment compares two scheduler-policy dispatch mechanisms under the same policy and workload:

1. compile-time template dispatch
2. runtime function-pointer dispatch

Both variants will use the same compiler, optimization level, QEMU configuration, and scheduler policy. The comparison will use:

- retired instructions via `rdinstret`
- cycle counts via `rdcycle`
- generated RISC-V assembly / symbol inspection

The purpose is **not to assume that templates are faster**, but to determine whether this abstraction introduces observable cost in the scheduler hot path and to explain the generated code when the result differs from expectation.

Measurement support is already wired into the kernel: `r_cycle()` and `r_instret()` read the corresponding CSRs, and machine-mode setup enables the counter bits needed for supervisor-mode access.

## mmap Type-Safety Experiment

`sys_mmap` was ported from C to C++ without intentionally changing its behavior. This experiment is separate from the scheduler performance work: its purpose is to examine where stronger compile-time checks are useful in low-level code.

The current port replaces selected C idioms with:

```text
PGROUNDUP macro            -> constexpr function
indexed VMA iteration      -> range-based for
field-by-field assignment  -> designated initialization
```

This part of the project is therefore about **type/structure safety**, not a performance claim.

## Toolchain and Build

The Makefile is configured for a GCC 14 RISC-V cross toolchain and C++23 kernel sources. C++ files are compiled with freestanding constraints including `-ffreestanding`, `-fno-exceptions`, and `-fno-rtti`.

Requirements:

- RISC-V GCC/G++ 14 cross toolchain
- QEMU >= 7.2

Run xv6:

```bash
make qemu
```

Run GCC static analysis for the C++ kernel files:

```bash
make analyze
```

The current Makefile also emits disassembly and symbol files for the kernel, which will be used for the dispatch experiment.

## Roadmap

Near-term work is intentionally limited to validating the scheduler experiment before expanding the project further:

- complete CFS integration
- implement the function-pointer baseline
- collect `rdcycle` / `rdinstret` measurements
- compare generated RISC-V code and report the result

Longer development notes and discarded design ideas are kept in [`plan.md`](plan.md); they should not be interpreted as completed features.

## Base Project and License

This repository is based on [MIT xv6-riscv](https://github.com/mit-pdos/xv6-riscv), a teaching operating system used in MIT 6.1810.

The original xv6 copyright and license are preserved in [`LICENSE`](LICENSE).
