// NS16550A UART: polled transmit, interrupt-driven receive.
#include "defs.h"

#define RHR 0   // receive holding (read)
#define THR 0   // transmit holding (write)
#define IER 1
#define FCR 2
#define LCR 3
#define LSR 5
#define LSR_RX_READY (1 << 0)
#define LSR_TX_IDLE  (1 << 5)

static inline volatile u8 *uart_reg(int r) {
  return (volatile u8 *)P2V(plat.uart_base) + r;
}

void uart_init(void) {
  *uart_reg(IER) = 0x00;   // interrupts off while configuring
  *uart_reg(LCR) = 0x80;   // DLAB: set baud rate divisor
  *uart_reg(0) = 0x03;     // 38.4k
  *uart_reg(1) = 0x00;
  *uart_reg(LCR) = 0x03;   // 8 data bits, no parity, one stop bit
  *uart_reg(FCR) = 0x07;   // enable and clear FIFOs
  *uart_reg(IER) = 0x01;   // receive interrupts
}

void uart_putc_sync(int c) {
  while ((*uart_reg(LSR) & LSR_TX_IDLE) == 0) {}
  *uart_reg(THR) = (u8)c;
}

void uart_intr(void) {
  while (*uart_reg(LSR) & LSR_RX_READY) console_intr(*uart_reg(RHR));
}
