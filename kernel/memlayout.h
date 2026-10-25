// Virtual address-space layout.
//
// Sv39 splits the 64-bit space in two canonical halves. User space owns the
// lower half; the kernel lives in the upper half and is mapped into every
// process's page table (without PTE_U), so traps never switch satp.
//
//   0x0000000000000000  unmapped (null pointer guard, 64 KiB)
//   0x0000000000010000  user program text, data, bss (from the ELF)
//                       heap, grown by sbrk() and faulted in lazily
//   0x0000003FFFEFF000  user stack (up to 1 MiB, grows down on demand)
//   0x0000003FFFFFF000  guard page
//   ---- non-canonical hole ----
//   0xFFFFFFC000000000  KERNBASE: direct map of all physical memory
//   0xFFFFFFC080200000  kernel image (loaded by OpenSBI at PA 0x80200000)
//   0xFFFFFFD000000000  kernel stacks, each below an unmapped guard page
#pragma once

#include "param.h"
#include "riscv.h"

#ifdef __ASSEMBLER__
#define _UL(x) x
#else
#define _UL(x) x##UL
#endif

#define KERNBASE      _UL(0xFFFFFFC000000000)
#define KERNEL_PHYS   _UL(0x80200000)
#define KSTACK_BASE   _UL(0xFFFFFFD000000000)
#define KSTACK_SIZE   (KSTACK_PAGES * PGSIZE)
// Bottom of the kernel stack for process slot i; the page below is a guard.
#define KSTACK(i) (KSTACK_BASE + (u64)(i) * (KSTACK_SIZE + PGSIZE) + PGSIZE)

#define USER_BASE     _UL(0x10000)
#define USER_TOP      _UL(0x4000000000)            // first non-canonical lower-half VA
#define USTACK_TOP    (USER_TOP - PGSIZE)       // leave a guard page at the very top
#define USTACK_BOTTOM (USTACK_TOP - USTACK_MAX)
#define HEAP_LIMIT    (USTACK_BOTTOM - PGSIZE)  // sbrk never reaches the stack

#ifndef __ASSEMBLER__
#define P2V(pa) ((void *)((u64)(pa) + KERNBASE))
#define V2P(va) ((u64)(va) - KERNBASE)
#endif
