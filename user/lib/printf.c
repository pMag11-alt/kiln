// Formatted and buffered output. stdout is line-buffered when it is the
// console and block-buffered otherwise; stderr is unbuffered.
#include "user.h"

struct outbuf {
  int fd;
  int len;
  char data[512];
};

static struct outbuf out1 = {1, 0, {0}};
static int out1_tty = -1;   // unknown until the first write

static void flush_buf(struct outbuf *b) {
  int off = 0;
  while (off < b->len) {
    ssize_t n = write(b->fd, b->data + off, (size_t)(b->len - off));
    if (n <= 0) break;
    off += (int)n;
  }
  b->len = 0;
}

void fflush(int fd) {
  if (fd == 1) flush_buf(&out1);
}

static void buf_putc(char c, void *arg) {
  struct outbuf *b = arg;
  b->data[b->len++] = c;
  if (b->len == (int)sizeof(b->data)) flush_buf(b);
}

static void stdout_putc(char c, void *arg) {
  (void)arg;
  if (out1_tty < 0) out1_tty = isatty(1);
  buf_putc(c, &out1);
  if (c == '\n' && out1_tty) flush_buf(&out1);
}

int vdprintf(int fd, const char *fmt, va_list ap) {
  if (fd == 1) return vformat(stdout_putc, NULL, fmt, ap);
  // Other descriptors: format into a private buffer and write it whole, so
  // concurrent writers do not interleave mid-line.
  if (fd == 2) fflush(1);
  struct outbuf b = {fd, 0, {0}};
  int n = vformat(buf_putc, &b, fmt, ap);
  flush_buf(&b);
  return n;
}

int printf(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int n = vdprintf(1, fmt, ap);
  va_end(ap);
  return n;
}

int dprintf(int fd, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int n = vdprintf(fd, fmt, ap);
  va_end(ap);
  return n;
}

int fputs(const char *s, int fd) {
  return dprintf(fd, "%s", s);
}

int puts(const char *s) {
  return printf("%s\n", s);
}

int putchar(int c) {
  stdout_putc((char)c, NULL);
  return c;
}

// Flush stdout on exit. crt0 calls exit() after main returns; programs
// that call exit() directly go through the same path.
void __flush_stdout(void) {
  flush_buf(&out1);
}

void reader_init(struct reader *r, int fd) {
  r->fd = fd;
  r->len = r->pos = 0;
  r->eof = false;
}

int reader_getline(struct reader *r, char *line, int max) {
  int n = 0;
  for (;;) {
    if (r->pos == r->len) {
      if (r->eof) break;
      fflush(1);   // prompts printed without a newline must show first
      ssize_t got = read(r->fd, r->buf, sizeof(r->buf));
      if (got <= 0) {
        r->eof = true;
        break;
      }
      r->len = (int)got;
      r->pos = 0;
    }
    char c = r->buf[r->pos++];
    if (c == '\n') {
      line[n] = '\0';
      return n;
    }
    if (n < max - 1) line[n++] = c;
  }
  line[n] = '\0';
  return n > 0 ? n : -1;
}
