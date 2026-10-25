// Trap dispatch: syscalls, device interrupts, timer ticks and user faults.
#include "defs.h"

volatile u64 ticks;
struct spinlock tickslock;
static u64 timer_interval;

enum { INTR_NONE, INTR_DEVICE, INTR_TIMER };

void trapinithart(void) {
  csr_write(stvec, (u64)trap_vector);
  csr_write(sscratch, 0);
  csr_set(sie, SIE_SEIE | SIE_STIE | SIE_SSIE);
}

void timer_inithart(void) {
  if (!timer_interval) {
    initlock(&tickslock, "ticks");
    timer_interval = plat.timebase / TICK_HZ;
  }
  sbi_set_timer(r_time() + timer_interval);
}

u64 uptime_ms(void) {
  return r_time() / (plat.timebase / 1000);
}

static int devintr(u64 scause) {
  if (!(scause & SCAUSE_INTR)) return INTR_NONE;
  u64 code = scause & ~SCAUSE_INTR;

  if (code == IRQ_S_EXT) {
    int irq = plic_claim();
    if (irq == (int)plat.uart_irq) uart_intr();
    else if (virtio_blk_owns_irq(irq)) virtio_blk_intr();
    else if (irq) kprintf("kiln: unexpected irq %d\n", irq);
    if (irq) plic_complete(irq);
    return INTR_DEVICE;
  }

  if (code == IRQ_S_TIMER) {
    if (panicking) {
      intr_off();
      for (;;) wfi();
    }
    sbi_set_timer(r_time() + timer_interval);
    struct cpu *c = mycpu();
    if (c->id == 0) {
      acquire(&tickslock);
      ticks++;
      wakeup((void *)&ticks);
      release(&tickslock);
    }
    if (c->proc) c->proc->ticks++;
    return INTR_TIMER;
  }

  if (code == IRQ_S_SOFT) {
    csr_clear(sip, SIE_SSIE);
    return INTR_DEVICE;
  }
  return INTR_NONE;
}

static const char *exc_name(u64 cause) {
  static const char *names[] = {
    [EXC_INST_MISALIGNED] = "misaligned fetch",
    [EXC_INST_ACCESS] = "instruction access fault",
    [EXC_ILLEGAL_INST] = "illegal instruction",
    [EXC_BREAKPOINT] = "breakpoint",
    [EXC_LOAD_MISALIGNED] = "misaligned load",
    [EXC_LOAD_ACCESS] = "load access fault",
    [EXC_STORE_MISALIGNED] = "misaligned store",
    [EXC_STORE_ACCESS] = "store access fault",
    [EXC_ECALL_U] = "ecall",
    [EXC_INST_PAGE_FAULT] = "instruction page fault",
    [EXC_LOAD_PAGE_FAULT] = "load page fault",
    [EXC_STORE_PAGE_FAULT] = "store page fault",
  };
  if (cause < ARRAY_SIZE(names) && names[cause]) return names[cause];
  return "unknown exception";
}

static bool quiet_faults(void) {
  return strstr(plat.bootargs, "kiln.quiet_faults") != NULL;
}

// Every trap from user mode lands here, on the process's kernel stack.
void usertrap(void) {
  struct proc *p = myproc();
  u64 scause = csr_read(scause);
  u64 stval = csr_read(stval);
  int which = INTR_NONE;

  if (csr_read(sstatus) & SSTATUS_SPP) panic("usertrap: not from user mode");

  if (scause == EXC_ECALL_U) {
    int k = killed(p);
    if (k) proc_exit(k);
    p->tf.epc += 4;
    intr_on();
    syscall();
  } else if ((which = devintr(scause)) != INTR_NONE) {
    // handled
  } else if (scause == EXC_INST_PAGE_FAULT || scause == EXC_LOAD_PAGE_FAULT ||
             scause == EXC_STORE_PAGE_FAULT) {
    intr_on();
    int r = handle_user_fault(p, stval, scause);
    if (r == -ENOMEM) {
      kprintf("kiln: pid %d (%s) killed: out of memory\n", p->pid, p->name);
      setkilled(p, SIGKILL);
    } else if (r < 0) {
      if (!quiet_faults())
        kprintf("kiln: pid %d (%s): segfault at %p (%s), pc %p\n", p->pid, p->name,
                (void *)stval, exc_name(scause), (void *)p->tf.epc);
      setkilled(p, SIGSEGV);
    }
  } else {
    int sig = scause == EXC_ILLEGAL_INST ? SIGILL
            : (scause == EXC_LOAD_MISALIGNED || scause == EXC_STORE_MISALIGNED) ? SIGBUS
            : SIGSEGV;
    if (!quiet_faults())
      kprintf("kiln: pid %d (%s): %s at pc %p, stval %p\n", p->pid, p->name,
              exc_name(scause), (void *)p->tf.epc, (void *)stval);
    setkilled(p, sig);
  }

  int k = killed(p);
  if (k) proc_exit(k);
  if (which == INTR_TIMER) yield();
  usertrapret();
}

void usertrapret(void) {
  struct proc *p = myproc();
  intr_off();
  u64 s = csr_read(sstatus);
  s &= ~(SSTATUS_SPP | SSTATUS_SUM | SSTATUS_FS);   // user mode, FPU off
  s |= SSTATUS_SPIE;                                // interrupts on in user mode
  csr_write(sstatus, s);
  csr_write(sepc, p->tf.epc);
  p->tf.kernel_sp = p->kstack + KSTACK_SIZE;
  p->tf.kernel_tp = r_tp();
  userret(&p->tf);
}

// Traps taken in supervisor mode: device interrupts, timer ticks (which may
// preempt the current kernel thread), or a kernel bug.
void kerneltrap(void) {
  u64 sepc = csr_read(sepc);
  u64 sstatus = csr_read(sstatus);
  u64 scause = csr_read(scause);

  if (!(sstatus & SSTATUS_SPP)) panic("kerneltrap: not from supervisor mode");
  if (intr_get()) panic("kerneltrap: interrupts enabled");

  int which = devintr(scause);
  if (which == INTR_NONE) {
    panic("%s in kernel at pc %p, stval %p (scause %#lx)", exc_name(scause),
          (void *)sepc, (void *)csr_read(stval), scause);
  }
  if (which == INTR_TIMER) {
    struct proc *p = myproc();
    if (p && p->state == RUNNING) yield();
  }
  // yield() may have run other code that took traps; restore ours.
  csr_write(sepc, sepc);
  csr_write(sstatus, sstatus);
}
