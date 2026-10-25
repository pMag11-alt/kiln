// A first-fit allocator over sbrk() with an address-ordered free list, so
// neighbouring free blocks coalesce.
#include "user.h"

typedef struct header {
  struct header *next;   // next free block (only while free)
  size_t size;           // block size in units, header included
} header;

#define UNIT sizeof(header)

static header base;
static header *freep;

static void *more_core(size_t units) {
  if (units < 4096 / UNIT) units = 4096 / UNIT;
  char *p = sbrk((long)(units * UNIT));
  if (p == (char *)-1) return NULL;
  header *h = (header *)p;
  h->size = units;
  free(h + 1);
  return freep;
}

void *malloc(size_t n) {
  if (n == 0) n = 1;
  if (n > (1UL << 40)) {
    errno = ENOMEM;
    return NULL;
  }
  size_t units = (n + UNIT - 1) / UNIT + 1;
  if (!freep) {
    base.next = freep = &base;
    base.size = 0;
  }
  header *prev = freep;
  for (header *p = prev->next;; prev = p, p = p->next) {
    if (p->size >= units) {
      if (p->size == units) {
        prev->next = p->next;
      } else {
        p->size -= units;     // carve from the tail
        p += p->size;
        p->size = units;
      }
      freep = prev;
      return p + 1;
    }
    if (p == freep && !more_core(units)) {
      errno = ENOMEM;
      return NULL;
    }
  }
}

void free(void *ptr) {
  if (!ptr) return;
  header *b = (header *)ptr - 1;
  header *p = freep;
  for (; !(b > p && b < p->next); p = p->next)
    if (p >= p->next && (b > p || b < p->next)) break;   // at either end of the arena
  if (b + b->size == p->next) {
    b->size += p->next->size;
    b->next = p->next->next;
  } else {
    b->next = p->next;
  }
  if (p + p->size == b) {
    p->size += b->size;
    p->next = b->next;
  } else {
    p->next = b;
  }
  freep = p;
}

void *calloc(size_t n, size_t size) {
  if (size && n > (size_t)-1 / size) {
    errno = ENOMEM;
    return NULL;
  }
  void *p = malloc(n * size);
  if (p) memset(p, 0, n * size);
  return p;
}

void *realloc(void *ptr, size_t n) {
  if (!ptr) return malloc(n);
  header *h = (header *)ptr - 1;
  size_t have = (h->size - 1) * UNIT;
  if (n <= have) return ptr;
  void *q = malloc(n);
  if (!q) return NULL;
  memcpy(q, ptr, have);
  free(ptr);
  return q;
}
