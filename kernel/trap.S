# Trap entry/exit and context switching.
#
# One stvec serves traps from both modes. sscratch tells them apart:
#   in user mode   sscratch = &proc->tf (a kernel address)
#   in kernel mode sscratch = 0
# Because the kernel half is mapped in every page table, entering the kernel
# needs no satp switch and no trampoline page.

#define TF_EPC        248
#define TF_KERNEL_SP  256
#define TF_KERNEL_TP  264
#define REG(n) ((n) - 1) * 8

    .section .text
    .globl trap_vector
    .balign 4
trap_vector:
    csrrw   tp, sscratch, tp
    bnez    tp, .Lfrom_user

    # From the kernel: tp was 0, so sscratch now holds the kernel's tp.
    # Swap it back and leave sscratch = 0.
    csrrw   tp, sscratch, zero

    addi    sp, sp, -256
    sd      ra, 0(sp)
    sd      gp, 8(sp)
    sd      t0, 16(sp)
    sd      t1, 24(sp)
    sd      t2, 32(sp)
    sd      a0, 40(sp)
    sd      a1, 48(sp)
    sd      a2, 56(sp)
    sd      a3, 64(sp)
    sd      a4, 72(sp)
    sd      a5, 80(sp)
    sd      a6, 88(sp)
    sd      a7, 96(sp)
    sd      t3, 104(sp)
    sd      t4, 112(sp)
    sd      t5, 120(sp)
    sd      t6, 128(sp)
    sd      s0, 136(sp)
    # Build a frame record so backtraces walk through the trap.
    csrr    t0, sepc
    sd      t0, 248(sp)
    sd      s0, 240(sp)
    addi    s0, sp, 256

    call    kerneltrap

    ld      ra, 0(sp)
    ld      gp, 8(sp)
    ld      t0, 16(sp)
    ld      t1, 24(sp)
    ld      t2, 32(sp)
    ld      a0, 40(sp)
    ld      a1, 48(sp)
    ld      a2, 56(sp)
    ld      a3, 64(sp)
    ld      a4, 72(sp)
    ld      a5, 80(sp)
    ld      a6, 88(sp)
    ld      a7, 96(sp)
    ld      t3, 104(sp)
    ld      t4, 112(sp)
    ld      t5, 120(sp)
    ld      t6, 128(sp)
    ld      s0, 136(sp)
    addi    sp, sp, 256
    sret

.Lfrom_user:
    # tp = trapframe, sscratch = user tp. Save every user register.
    sd      x1, REG(1)(tp)
    sd      x2, REG(2)(tp)
    sd      x3, REG(3)(tp)
    sd      x5, REG(5)(tp)
    sd      x6, REG(6)(tp)
    sd      x7, REG(7)(tp)
    sd      x8, REG(8)(tp)
    sd      x9, REG(9)(tp)
    sd      x10, REG(10)(tp)
    sd      x11, REG(11)(tp)
    sd      x12, REG(12)(tp)
    sd      x13, REG(13)(tp)
    sd      x14, REG(14)(tp)
    sd      x15, REG(15)(tp)
    sd      x16, REG(16)(tp)
    sd      x17, REG(17)(tp)
    sd      x18, REG(18)(tp)
    sd      x19, REG(19)(tp)
    sd      x20, REG(20)(tp)
    sd      x21, REG(21)(tp)
    sd      x22, REG(22)(tp)
    sd      x23, REG(23)(tp)
    sd      x24, REG(24)(tp)
    sd      x25, REG(25)(tp)
    sd      x26, REG(26)(tp)
    sd      x27, REG(27)(tp)
    sd      x28, REG(28)(tp)
    sd      x29, REG(29)(tp)
    sd      x30, REG(30)(tp)
    sd      x31, REG(31)(tp)
    csrr    t0, sscratch
    sd      t0, REG(4)(tp)
    csrr    t0, sepc
    sd      t0, TF_EPC(tp)
    csrw    sscratch, zero

    ld      sp, TF_KERNEL_SP(tp)
    ld      tp, TF_KERNEL_TP(tp)
    mv      fp, zero
    call    usertrap
    # usertrap never returns; it leaves through usertrapret -> userret.
1:  j       1b

# userret(struct trapframe *tf): return to user mode. Called with interrupts
# off and sepc/sstatus already prepared by usertrapret().
    .globl userret
userret:
    csrw    sscratch, a0
    ld      x1, REG(1)(a0)
    ld      x2, REG(2)(a0)
    ld      x3, REG(3)(a0)
    ld      x4, REG(4)(a0)
    ld      x5, REG(5)(a0)
    ld      x6, REG(6)(a0)
    ld      x7, REG(7)(a0)
    ld      x8, REG(8)(a0)
    ld      x9, REG(9)(a0)
    ld      x11, REG(11)(a0)
    ld      x12, REG(12)(a0)
    ld      x13, REG(13)(a0)
    ld      x14, REG(14)(a0)
    ld      x15, REG(15)(a0)
    ld      x16, REG(16)(a0)
    ld      x17, REG(17)(a0)
    ld      x18, REG(18)(a0)
    ld      x19, REG(19)(a0)
    ld      x20, REG(20)(a0)
    ld      x21, REG(21)(a0)
    ld      x22, REG(22)(a0)
    ld      x23, REG(23)(a0)
    ld      x24, REG(24)(a0)
    ld      x25, REG(25)(a0)
    ld      x26, REG(26)(a0)
    ld      x27, REG(27)(a0)
    ld      x28, REG(28)(a0)
    ld      x29, REG(29)(a0)
    ld      x30, REG(30)(a0)
    ld      x31, REG(31)(a0)
    ld      x10, REG(10)(a0)
    sret

# swtch(struct context *old, struct context *new): save callee-saved
# registers into old, load them from new, and return on new's stack.
    .globl swtch
swtch:
    sd      ra, 0(a0)
    sd      sp, 8(a0)
    sd      s0, 16(a0)
    sd      s1, 24(a0)
    sd      s2, 32(a0)
    sd      s3, 40(a0)
    sd      s4, 48(a0)
    sd      s5, 56(a0)
    sd      s6, 64(a0)
    sd      s7, 72(a0)
    sd      s8, 80(a0)
    sd      s9, 88(a0)
    sd      s10, 96(a0)
    sd      s11, 104(a0)

    ld      ra, 0(a1)
    ld      sp, 8(a1)
    ld      s0, 16(a1)
    ld      s1, 24(a1)
    ld      s2, 32(a1)
    ld      s3, 40(a1)
    ld      s4, 48(a1)
    ld      s5, 56(a1)
    ld      s6, 64(a1)
    ld      s7, 72(a1)
    ld      s8, 80(a1)
    ld      s9, 88(a1)
    ld      s10, 96(a1)
    ld      s11, 104(a1)
    ret
