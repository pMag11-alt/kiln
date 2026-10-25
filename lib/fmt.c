// A small printf engine. The kernel feeds it the UART; user space feeds it a
// buffered writer.
#include "klib.h"

struct spec {
  bool left;
  bool zero;
  bool alt;   // '#': 0x prefix for hex, leading 0 for octal
  int width;
  int prec;   // -1 when absent
};

static int pad(fmt_putc put, void *arg, char c, int n) {
  for (int i = 0; i < n; i++) put(c, arg);
  return n > 0 ? n : 0;
}

static int emit_str(fmt_putc put, void *arg, const char *s, size_t len,
                    const struct spec *sp) {
  int out = 0;
  int padding = sp->width > (int)len ? sp->width - (int)len : 0;
  if (!sp->left) out += pad(put, arg, ' ', padding);
  for (size_t i = 0; i < len; i++) put(s[i], arg);
  out += (int)len;
  if (sp->left) out += pad(put, arg, ' ', padding);
  return out;
}

static int emit_num(fmt_putc put, void *arg, uint64_t v, unsigned base,
                    bool neg, bool upper, const char *prefix,
                    const struct spec *sp) {
  const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
  char tmp[24];
  int n = 0;
  do {
    tmp[n++] = digits[v % base];
    v /= base;
  } while (v);
  int ndigits = n;
  if (sp->prec > ndigits) ndigits = sp->prec;
  int plen = (neg ? 1 : 0) + (int)strlen(prefix);
  int total = plen + ndigits;
  int padding = sp->width > total ? sp->width - total : 0;
  int out = 0;
  bool zeropad = sp->zero && !sp->left && sp->prec < 0;
  if (!sp->left && !zeropad) out += pad(put, arg, ' ', padding);
  if (neg) { put('-', arg); out++; }
  for (const char *p = prefix; *p; p++) { put(*p, arg); out++; }
  if (zeropad) out += pad(put, arg, '0', padding);
  out += pad(put, arg, '0', ndigits - n);
  while (n) { put(tmp[--n], arg); out++; }
  if (sp->left) out += pad(put, arg, ' ', padding);
  return out;
}

int vformat(fmt_putc put, void *arg, const char *fmt, va_list ap) {
  int out = 0;
  for (; *fmt; fmt++) {
    if (*fmt != '%') {
      put(*fmt, arg);
      out++;
      continue;
    }
    const char *start = fmt++;
    struct spec sp = {false, false, false, 0, -1};
    for (;; fmt++) {
      if (*fmt == '-') sp.left = true;
      else if (*fmt == '0') sp.zero = true;
      else if (*fmt == '#') sp.alt = true;
      else break;
    }
    if (*fmt == '*') {
      sp.width = va_arg(ap, int);
      if (sp.width < 0) { sp.left = true; sp.width = -sp.width; }
      fmt++;
    } else {
      while (*fmt >= '0' && *fmt <= '9') sp.width = sp.width * 10 + (*fmt++ - '0');
    }
    if (*fmt == '.') {
      fmt++;
      sp.prec = 0;
      if (*fmt == '*') {
        sp.prec = va_arg(ap, int);
        fmt++;
      } else {
        while (*fmt >= '0' && *fmt <= '9') sp.prec = sp.prec * 10 + (*fmt++ - '0');
      }
    }
    int lng = 0;
    while (*fmt == 'l') { lng++; fmt++; }
    if (*fmt == 'z') { lng = 1; fmt++; }

    switch (*fmt) {
    case 'd':
    case 'i': {
      int64_t v = lng ? va_arg(ap, long) : va_arg(ap, int);
      uint64_t mag = v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
      out += emit_num(put, arg, mag, 10, v < 0, false, "", &sp);
      break;
    }
    case 'u':
    case 'x':
    case 'X':
    case 'o': {
      uint64_t v = lng ? va_arg(ap, unsigned long) : va_arg(ap, unsigned int);
      unsigned base = *fmt == 'u' ? 10 : *fmt == 'o' ? 8 : 16;
      const char *prefix = "";
      if (sp.alt && v) prefix = base == 16 ? (*fmt == 'X' ? "0X" : "0x") : base == 8 ? "0" : "";
      out += emit_num(put, arg, v, base, false, *fmt == 'X', prefix, &sp);
      break;
    }
    case 'p': {
      uint64_t v = (uint64_t)(uintptr_t)va_arg(ap, void *);
      out += emit_num(put, arg, v, 16, false, false, "0x", &sp);
      break;
    }
    case 's': {
      const char *s = va_arg(ap, const char *);
      if (!s) s = "(null)";
      size_t len = sp.prec >= 0 ? strnlen(s, (size_t)sp.prec) : strlen(s);
      out += emit_str(put, arg, s, len, &sp);
      break;
    }
    case 'c': {
      char c = (char)va_arg(ap, int);
      out += emit_str(put, arg, &c, 1, &sp);
      break;
    }
    case '%':
      put('%', arg);
      out++;
      break;
    case '\0':
      // A lone '%' at the end: print it and stop.
      put('%', arg);
      return out + 1;
    default:
      // Unknown conversion: print it literally.
      for (const char *p = start; p <= fmt; p++) { put(*p, arg); out++; }
      break;
    }
  }
  return out;
}

struct strbuf {
  char *buf;
  size_t size;
  size_t len;
};

static void strbuf_putc(char c, void *arg) {
  struct strbuf *sb = arg;
  if (sb->len + 1 < sb->size) sb->buf[sb->len] = c;
  sb->len++;
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap) {
  struct strbuf sb = {buf, size, 0};
  int n = vformat(strbuf_putc, &sb, fmt, ap);
  if (size) buf[sb.len < size ? sb.len : size - 1] = '\0';
  return n;
}

int snprintf(char *buf, size_t size, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, size, fmt, ap);
  va_end(ap);
  return n;
}
