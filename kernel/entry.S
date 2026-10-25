# Boot entry. OpenSBI starts the boot hart here in S-mode with the MMU off,
# at the physical load address 0x80200000:
#   a0 = hart id, a1 = physical address of the flattened device tree.
# Secondary harts arrive at _secondary_start through the SBI HSM extension.
#
# The kernel is linked in the upper half (KERNBASE + physical), so before any
# C runs we turn on Sv39 with a tiny static page table that maps
#   VA 0x80000000            -> PA 0x80000000   (identity, for the next fetch)
#   VA KERNBASE + [0, 4 GiB) -> PA [0, 4 GiB)   (the kernel's direct map)
# with 1 GiB pages, then jump to the linked address. Everything up to that
# jump uses only PC-relative addressing, so it runs fine at the physical PC.

#include "param.h"
#include "memlayout.h"

    .section .text.boot, "ax"
    .globl _start
_start:
    csrw    sie, zero
    lla     s2, .Lboot_target

    # Zero .bss. Only the boot hart runs this, before anyone else is awake.
    lla     t0, __bss_start
    lla     t1, __bss_end
1:  bgeu    t0, t1, .Lcommon
    sd      zero, 0(t0)
    addi    t0, t0, 8
    j       1b

    .globl _secondary_start
_secondary_start:
    csrw    sie, zero
    lla     s2, .Lsecondary_target

.Lcommon:
    # Per-hart boot stack: boot_stacks + (hartid + 1) * BOOT_STACK_SIZE.
    li      t0, NCPU
    bgeu    a0, t0, .Lpark
    lla     sp, boot_stacks
    addi    t0, a0, 1
    li      t1, BOOT_STACK_SIZE
    mul     t0, t0, t1
    add     sp, sp, t0

    # satp = Sv39 | PPN(boot_pagetable)
    lla     t0, boot_pagetable
    srli    t0, t0, 12
    li      t1, 0x8000000000000000
    or      t0, t0, t1
    sfence.vma
    csrw    satp, t0
    sfence.vma

    # Move the stack to the direct map and jump to the high-half C entry.
    li      t0, KERNBASE
    add     sp, sp, t0
    ld      t1, 0(s2)
    mv      fp, zero
    jr      t1

.Lpark:
    wfi
    j       .Lpark

    .section .rodata
    .balign 8
.Lboot_target:
    .dword  kmain
.Lsecondary_target:
    .dword  kmain_secondary

# Boot page table: 1 GiB leaf PTEs with V|R|W|X|A|D set.
#define GIGA_PTE(pa) ((((pa) >> 12) << 10) | 0xCF)
    .section .data
    .balign 4096
    .globl boot_pagetable
boot_pagetable:
    .dword  0
    .dword  0
    .dword  GIGA_PTE(0x80000000)        # index 2: identity map of RAM
    .zero   8 * (256 - 3)
    .dword  GIGA_PTE(0x00000000)        # index 256: KERNBASE + 0 GiB
    .dword  GIGA_PTE(0x40000000)
    .dword  GIGA_PTE(0x80000000)
    .dword  GIGA_PTE(0xC0000000)
    .zero   8 * (512 - 260)
