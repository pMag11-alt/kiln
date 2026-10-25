// The console: a canonical-mode line discipline over the UART.
//
//   Backspace/DEL  erase a character      ^U  erase the line
//   ^D             end of file            ^C  SIGINT to the foreground group
//   ^P             dump the process table (debugging)
#include "defs.h"

#define INPUT_BUF 256
#define CTRL(x) ((x) - '@')

static struct {
  struct spinlock lock;
  char buf[INPUT_BUF];
  u32 r;     // next byte read() returns
  u32 w;     // end of committed (newline-terminated) input
  u32 e;     // end of the line being edited
  int fg_pgid;
} cons;

static struct spinlock out_lock;

static void echo(int c) {
  if (c == '\b') {
    uart_putc_sync('\b');
    uart_putc_sync(' ');
    uart_putc_sync('\b');
  } else {
    uart_putc_sync(c);
  }
}

void console_intr(int c) {
  acquire(&cons.lock);
  switch (c) {
  case CTRL('C'): {
    cons.e = cons.w;   // drop the partial line
    int pg = cons.fg_pgid;
    release(&cons.lock);
    uart_putc_sync('^');
    uart_putc_sync('C');
    uart_putc_sync('\n');
    if (pg > 0) proc_kill(-pg, SIGINT);
    return;
  }
  case CTRL('P'):
    release(&cons.lock);
    procdump();
    return;
  case CTRL('U'):
    while (cons.e != cons.w && cons.buf[(cons.e - 1) % INPUT_BUF] != '\n') {
      cons.e--;
      echo('\b');
    }
    break;
  case CTRL('H'):
  case 0x7f:
    if (cons.e != cons.w) {
      cons.e--;
      echo('\b');
    }
    break;
  default:
    if (c == 0 || cons.e - cons.r >= INPUT_BUF) break;
    if (c == '\r') c = '\n';
    cons.buf[cons.e++ % INPUT_BUF] = (char)c;
    if (c != CTRL('D')) echo(c);
    if (c == '\n' || c == CTRL('D') || cons.e - cons.r == INPUT_BUF) {
      cons.w = cons.e;
      wakeup(&cons.r);
    }
    break;
  }
  release(&cons.lock);
}

static i64 console_read(int minor, u64 udst, u64 n) {
  (void)minor;
  struct proc *p = myproc();
  u64 done = 0;
  char kbuf[64];
  acquire(&cons.lock);
  while (done < n) {
    while (cons.r == cons.w) {
      if (killed(p)) {
        release(&cons.lock);
        return done ? (i64)done : -EINTR;
      }
      sleep(&cons.r, &cons.lock);
    }
    // Pull bytes up to the end of a line into kbuf.
    u64 m = 0;
    bool eol = false;
    while (cons.r != cons.w && done + m < n && m < sizeof(kbuf)) {
      char c = cons.buf[cons.r % INPUT_BUF];
      if (c == CTRL('D')) {
        // EOF: consume it only if nothing has been read in this call, so
        // "abc^D" returns "abc" now and 0 on the next read.
        if (done + m == 0) cons.r++;
        eol = true;
        break;
      }
      cons.r++;
      kbuf[m++] = c;
      if (c == '\n') { eol = true; break; }
    }
    release(&cons.lock);
    if (m && copyout(p, udst + done, kbuf, m) < 0) return done ? (i64)done : -EFAULT;
    done += m;
    if (eol) return (i64)done;
    acquire(&cons.lock);
  }
  release(&cons.lock);
  return (i64)done;
}

static i64 console_write(int minor, u64 usrc, u64 n) {
  (void)minor;
  struct proc *p = myproc();
  char kbuf[128];
  u64 done = 0;
  while (done < n) {
    u64 m = MIN(n - done, sizeof(kbuf));
    if (copyin(p, kbuf, usrc + done, m) < 0) return done ? (i64)done : -EFAULT;
    acquire(&out_lock);
    for (u64 i = 0; i < m; i++) uart_putc_sync(kbuf[i]);
    release(&out_lock);
    done += m;
  }
  return (i64)done;
}

void console_set_fg(int pgid) {
  acquire(&cons.lock);
  cons.fg_pgid = pgid;
  release(&cons.lock);
}

void console_init(void) {
  initlock(&cons.lock, "cons");
  initlock(&out_lock, "cons_out");
  uart_init();
  devsw[MAJOR_CONSOLE].read = console_read;
  devsw[MAJOR_CONSOLE].write = console_write;
}
