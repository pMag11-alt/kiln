// Freestanding string and memory routines, linked into both the kernel and
// user programs. The compiler may emit calls to memcpy/memset/memmove on its
// own, so these must exist even when no code calls them by name.
#include "klib.h"

void *memset(void *dst, int c, size_t n) {
  unsigned char *d = dst;
  unsigned char b = (unsigned char)c;
  // Byte-fill up to an 8-byte boundary, then fill whole words.
  while (n && ((uintptr_t)d & 7)) { *d++ = b; n--; }
  if (n >= 8) {
    uint64_t w = b * 0x0101010101010101ULL;
    uint64_t *dw = (uint64_t *)d;
    for (; n >= 8; n -= 8) *dw++ = w;
    d = (unsigned char *)dw;
  }
  while (n--) *d++ = b;
  return dst;
}

void *memcpy(void *dst, const void *src, size_t n) {
  unsigned char *d = dst;
  const unsigned char *s = src;
  if ((((uintptr_t)d | (uintptr_t)s) & 7) == 0) {
    uint64_t *dw = (uint64_t *)d;
    const uint64_t *sw = (const uint64_t *)s;
    for (; n >= 8; n -= 8) *dw++ = *sw++;
    d = (unsigned char *)dw;
    s = (const unsigned char *)sw;
  }
  while (n--) *d++ = *s++;
  return dst;
}

void *memmove(void *dst, const void *src, size_t n) {
  unsigned char *d = dst;
  const unsigned char *s = src;
  if (d == s || n == 0) return dst;
  if (d < s || d >= s + n) return memcpy(dst, src, n);
  d += n;
  s += n;
  while (n--) *--d = *--s;
  return dst;
}

int memcmp(const void *a, const void *b, size_t n) {
  const unsigned char *x = a, *y = b;
  for (; n; n--, x++, y++)
    if (*x != *y) return *x - *y;
  return 0;
}

void *memchr(const void *s, int c, size_t n) {
  const unsigned char *p = s;
  for (; n; n--, p++)
    if (*p == (unsigned char)c) return (void *)p;
  return NULL;
}

size_t strlen(const char *s) {
  const char *p = s;
  while (*p) p++;
  return p - s;
}

size_t strnlen(const char *s, size_t max) {
  size_t n = 0;
  while (n < max && s[n]) n++;
  return n;
}

int strcmp(const char *a, const char *b) {
  while (*a && *a == *b) a++, b++;
  return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n) {
  for (; n; n--, a++, b++) {
    if (*a != *b) return (unsigned char)*a - (unsigned char)*b;
    if (!*a) return 0;
  }
  return 0;
}

char *strcpy(char *dst, const char *src) {
  char *d = dst;
  while ((*d++ = *src++)) {}
  return dst;
}

// Copies at most size-1 bytes and always NUL-terminates (when size > 0).
// Returns strlen(src) so callers can detect truncation.
size_t strlcpy(char *dst, const char *src, size_t size) {
  size_t len = strlen(src);
  if (size) {
    size_t n = len < size - 1 ? len : size - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
  }
  return len;
}

size_t strlcat(char *dst, const char *src, size_t size) {
  size_t dl = strnlen(dst, size);
  if (dl == size) return size + strlen(src);
  return dl + strlcpy(dst + dl, src, size - dl);
}

char *strcat(char *dst, const char *src) {
  strcpy(dst + strlen(dst), src);
  return dst;
}

char *strchr(const char *s, int c) {
  for (;; s++) {
    if (*s == (char)c) return (char *)s;
    if (!*s) return NULL;
  }
}

char *strrchr(const char *s, int c) {
  const char *last = NULL;
  for (;; s++) {
    if (*s == (char)c) last = s;
    if (!*s) return (char *)last;
  }
}

char *strstr(const char *hay, const char *needle) {
  size_t n = strlen(needle);
  if (!n) return (char *)hay;
  for (; *hay; hay++)
    if (*hay == *needle && strncmp(hay, needle, n) == 0) return (char *)hay;
  return NULL;
}
