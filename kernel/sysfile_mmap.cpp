// sys_mmap — Phase 1.5 (see plan.md): ported from kernel/sysfile.c to C++
// for static type safety. No behavior change from the original C version;
// only the four techniques documented in plan.md Phase 1.5 are applied:
//   [1] PGROUNDUP macro           -> constexpr uint64 page_round_up(uint64)
//   [2][3] positional field assigns -> designated initializer (order-checked)
//   [4] indexed loop `p->vmas[i]`  -> range-based for with `auto&`

extern "C" {
#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"
#include "fcntl.h"
#include "memlayout.h"
}

constexpr uint64 page_round_up(uint64 sz) {
    return (sz + PGSIZE - 1) & ~(PGSIZE - 1);
}

extern "C" uint64 sys_mmap(void)
{
    int length, prot, flags, fd, offset;
    struct file *f;
    struct proc *p = myproc();

    argint(0, &length);
    argint(1, &prot);
    argint(2, &flags);
    if (argfd(3, &fd, &f) < 0) { return -1; }
    argint(4, &offset);

    if (length <= 0 || fd < 0) { return -1; }

    // [4] find a free vma slot: index loop -> range-based for + auto&
    struct vma *free_vma = nullptr;
    for (auto &v : p->vmas) {
        if (!v.used) { free_vma = &v; break; }
    }
    if (!free_vma) { return -1; }

    // [1] macro -> constexpr, type-checked function
    const uint64 aligned_len = page_round_up(length);
    uint64 addr = TRAPFRAME - aligned_len;

    // address-collision avoidance: same O(N^2) restart-scan as the original,
    // just with an explicit `collision` flag instead of `j = -1; continue;`
    bool collision = true;
    while (collision) {
        collision = false;
        for (const auto &v : p->vmas) {
            if (!v.used) continue;
            if (addr < (v.addr + v.length) && (addr + aligned_len > v.addr)) {
                addr = v.addr - aligned_len;
                collision = true;
                break;
            }
        }
    }

    // [2][3] positional field assignment -> designated initializer
    // (must list fields in struct vma's declaration order, kernel/proc.h)
    *free_vma = vma{
        .used = 1,
        .addr = addr,
        .length = (uint64)length,   // brace-init forbids the narrowing int->uint64 conversion C silently allowed
        .prot = prot,
        .flags = flags,
        .offset = offset,
        .f = filedup(f)
    };

    return addr;
}
