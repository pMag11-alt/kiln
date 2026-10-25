// Kernel console output, panic and backtraces.
#include "defs.h"

volatile int panicking;
static struct spinlock pr_lock = {0, "printf", 0};

static void uart_sink(char c, void *arg) {
  (void)arg;
  uart_putc_sync(c);
}

void kprintf(const char *fmt, ...) {
  bool locking = !panicking;
  if (locking) acquire(&pr_lock);
  va_list ap;
  va_start(ap, fmt);
  vformat(uart_sink, NULL, fmt, ap);
  va_end(ap);
  if (locking) release(&pr_lock);
}

// Walk the frame-pointer chain. With -fno-omit-frame-pointer every frame
// stores its return address at fp-8 and the caller's fp at fp-16.
void backtrace(u64 fp) {
  kprintf("backtrace:\n");
  for (int depth = 0; depth < 24; depth++) {
    if (fp < KERNBASE || (fp & 7) || fp < 16) break;
    u64 ra = *(u64 *)(fp - 8);
    u64 prev = *(u64 *)(fp - 16);
    if (ra < KERNBASE) break;
    kprintf("  %p\n", (void *)ra);
    if (prev <= fp) break;   // stacks grow down; a frame record must move up
    fp = prev;
  }
}

void panic(const char *fmt, ...) {
  intr_off();
  // The first hart to panic prints; the others stop at their next tick.
  if (__atomic_exchange_n(&panicking, 1, __ATOMIC_SEQ_CST)) {
    for (;;) wfi();
  }
  kprintf("\nkernel panic on hart %d: ", (int)mycpu()->hartid);
  va_list ap;
  va_start(ap, fmt);
  vformat(uart_sink, NULL, fmt, ap);
  va_end(ap);
  kprintf("\n");
  backtrace(r_fp());
  kprintf("halting. Symbolize with: make addr2line ADDRS=\"...\"\n");
  for (;;) wfi();
}
