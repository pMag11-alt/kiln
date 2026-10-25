// RISC-V privileged-architecture helpers for supervisor mode.
#pragma once

#define PGSIZE  4096
#define PGSHIFT 12

// sstatus
#define SSTATUS_SIE  (1UL << 1)
#define SSTATUS_SPIE (1UL << 5)
#define SSTATUS_SPP  (1UL << 8)
#define SSTATUS_FS   (3UL << 13)
#define SSTATUS_SUM  (1UL << 18)

// sie / sip
#define SIE_SSIE (1UL << 1)
#define SIE_STIE (1UL << 5)
#define SIE_SEIE (1UL << 9)

// scause
#define SCAUSE_INTR      (1UL << 63)
#define IRQ_S_SOFT       1
#define IRQ_S_TIMER      5
#define IRQ_S_EXT        9
#define EXC_INST_MISALIGNED 0
#define EXC_INST_ACCESS     1
#define EXC_ILLEGAL_INST    2
#define EXC_BREAKPOINT      3
#define EXC_LOAD_MISALIGNED 4
#define EXC_LOAD_ACCESS     5
#define EXC_STORE_MISALIGNED 6
#define EXC_STORE_ACCESS    7
#define EXC_ECALL_U         8
#define EXC_INST_PAGE_FAULT 12
#define EXC_LOAD_PAGE_FAULT 13
#define EXC_STORE_PAGE_FAULT 15

// Sv39 page-table entries
#define PTE_V (1UL << 0)
#define PTE_R (1UL << 1)
#define PTE_W (1UL << 2)
#define PTE_X (1UL << 3)
#define PTE_U (1UL << 4)
#define PTE_G (1UL << 5)
#define PTE_A (1UL << 6)
#define PTE_D (1UL << 7)
#define PTE_COW (1UL << 8)   // software bit (RSW): copy-on-write page
#define PTE_FLAGS(pte) ((pte) & 0x3FF)
#define PTE_LEAF(pte)  ((pte) & (PTE_R | PTE_W | PTE_X))
#define PA2PTE(pa) ((((u64)(pa)) >> 12) << 10)
#define PTE2PA(pte) (((pte) >> 10) << 12)

#define SATP_SV39 (8UL << 60)
#define MAKE_SATP(pa) (SATP_SV39 | (((u64)(pa)) >> 12))

#define PXSHIFT(level) (PGSHIFT + 9 * (level))
#define PX(level, va) ((((u64)(va)) >> PXSHIFT(level)) & 0x1FF)

#ifndef __ASSEMBLER__
#include "types.h"

#define csr_read(csr) ({ u64 __v; asm volatile("csrr %0, " #csr : "=r"(__v)); __v; })
#define csr_write(csr, val) ({ u64 __v = (u64)(val); asm volatile("csrw " #csr ", %0" ::"rK"(__v) : "memory"); })
#define csr_set(csr, val) ({ u64 __v = (u64)(val); asm volatile("csrs " #csr ", %0" ::"rK"(__v) : "memory"); })
#define csr_clear(csr, val) ({ u64 __v = (u64)(val); asm volatile("csrc " #csr ", %0" ::"rK"(__v) : "memory"); })

static inline void intr_on(void) { csr_set(sstatus, SSTATUS_SIE); }
static inline void intr_off(void) { csr_clear(sstatus, SSTATUS_SIE); }
static inline bool intr_get(void) { return (csr_read(sstatus) & SSTATUS_SIE) != 0; }

static inline u64 r_tp(void) { u64 x; asm volatile("mv %0, tp" : "=r"(x)); return x; }
static inline void w_tp(u64 x) { asm volatile("mv tp, %0" ::"r"(x)); }
static inline u64 r_fp(void) { u64 x; asm volatile("mv %0, s0" : "=r"(x)); return x; }
static inline u64 r_time(void) { u64 x; asm volatile("rdtime %0" : "=r"(x)); return x; }

static inline void sfence_vma(void) { asm volatile("sfence.vma zero, zero" ::: "memory"); }
static inline void sfence_vma_addr(u64 va) { asm volatile("sfence.vma %0, zero" ::"r"(va) : "memory"); }
static inline void wfi(void) { asm volatile("wfi"); }
#endif
