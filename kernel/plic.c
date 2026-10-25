// Platform-level interrupt controller.
#include "defs.h"

#define PLIC_PRIORITY(irq)   (4 * (irq))
#define PLIC_ENABLE(ctx)     (0x2000 + 0x80 * (ctx))
#define PLIC_THRESHOLD(ctx)  (0x200000 + 0x1000 * (ctx))
#define PLIC_CLAIM(ctx)      (0x200004 + 0x1000 * (ctx))

static inline volatile u32 *plic_reg(u64 off) {
  return (volatile u32 *)((u8 *)P2V(plat.plic_base) + off);
}

// On QEMU virt each hart has an M-mode and an S-mode context: 2h, 2h+1.
static int s_context(void) {
  return 2 * (int)mycpu()->hartid + 1;
}

void plic_init(void) {
  *plic_reg(PLIC_PRIORITY(plat.uart_irq)) = 1;
  for (int i = 0; i < plat.nvirtio; i++) *plic_reg(PLIC_PRIORITY(plat.virtio_irq[i])) = 1;
}

void plic_inithart(void) {
  int ctx = s_context();
  u32 words[4] = {0};
  words[plat.uart_irq / 32] |= 1u << (plat.uart_irq % 32);
  for (int i = 0; i < plat.nvirtio; i++)
    words[plat.virtio_irq[i] / 32] |= 1u << (plat.virtio_irq[i] % 32);
  for (int w = 0; w < 4; w++) plic_reg(PLIC_ENABLE(ctx))[w] = words[w];
  *plic_reg(PLIC_THRESHOLD(ctx)) = 0;
}

int plic_claim(void) {
  return (int)*plic_reg(PLIC_CLAIM(s_context()));
}

void plic_complete(int irq) {
  *plic_reg(PLIC_CLAIM(s_context())) = (u32)irq;
}
