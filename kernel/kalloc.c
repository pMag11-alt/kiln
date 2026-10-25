// Physical page allocator with per-page reference counts.
//
// Every page between the end of the kernel image and the end of RAM is on a
// free list. Reference counts let copy-on-write fork share a page between
// processes: kfree() only returns a page to the list when its count drops
// to zero.
#include "defs.h"

extern char end[];   // first address after the kernel image (kernel.ld)

struct run {
  struct run *next;
};

static struct {
  struct spinlock lock;
  struct run *freelist;
  u64 nfree;
  u64 total;
} kmem;

static u16 *refcnt;        // one counter per physical page of RAM
static u64 ram_base_pa;
static u64 ram_end_pa;

static inline u64 page_index(void *kva) {
  u64 pa = V2P(kva);
  if (pa < ram_base_pa || pa >= ram_end_pa || (pa & (PGSIZE - 1)))
    panic("kalloc: bad page %p", kva);
  return (pa - ram_base_pa) >> PGSHIFT;
}

static bool reserved(u64 pa) {
  for (int i = 0; i < plat.nresv; i++)
    if (pa + PGSIZE > plat.resv_base[i] && pa < plat.resv_base[i] + plat.resv_size[i])
      return true;
  return false;
}

void kinit(void) {
  initlock(&kmem.lock, "kmem");
  ram_base_pa = plat.ram_base;
  ram_end_pa = plat.ram_base + plat.ram_size;

  // The refcount array sits right after the kernel image.
  u64 npages = (ram_end_pa - ram_base_pa) >> PGSHIFT;
  u64 start = ROUNDUP(V2P(end), PGSIZE);
  refcnt = P2V(start);
  start += ROUNDUP(npages * sizeof(u16), PGSIZE);
  memset(refcnt, 0, npages * sizeof(u16));

  for (u64 pa = start; pa + PGSIZE <= ram_end_pa; pa += PGSIZE) {
    if (reserved(pa)) continue;
    struct run *r = P2V(pa);
    r->next = kmem.freelist;
    kmem.freelist = r;
    kmem.nfree++;
    kmem.total++;
  }
}

void kfree(void *kva) {
  u64 idx = page_index(kva);
  acquire(&kmem.lock);
  if (refcnt[idx] == 0) panic("kfree: double free of %p", kva);
  if (--refcnt[idx] > 0) {
    release(&kmem.lock);
    return;
  }
  release(&kmem.lock);

  // Fill with junk to catch dangling references.
  memset(kva, 0x6b, PGSIZE);

  struct run *r = kva;
  acquire(&kmem.lock);
  r->next = kmem.freelist;
  kmem.freelist = r;
  kmem.nfree++;
  release(&kmem.lock);
}

void *kalloc(void) {
  acquire(&kmem.lock);
  struct run *r = kmem.freelist;
  if (r) {
    kmem.freelist = r->next;
    kmem.nfree--;
    refcnt[page_index(r)] = 1;
  }
  release(&kmem.lock);
  return r;
}

void *kzalloc(void) {
  void *p = kalloc();
  if (p) memset(p, 0, PGSIZE);
  return p;
}

void kref_inc(void *kva) {
  u64 idx = page_index(kva);
  acquire(&kmem.lock);
  if (refcnt[idx] == 0) panic("kref_inc: free page %p", kva);
  refcnt[idx]++;
  release(&kmem.lock);
}

int kref_get(void *kva) {
  u64 idx = page_index(kva);
  acquire(&kmem.lock);
  int n = refcnt[idx];
  release(&kmem.lock);
  return n;
}

u64 kmem_free_pages(void) {
  return __atomic_load_n(&kmem.nfree, __ATOMIC_RELAXED);
}

u64 kmem_total_pages(void) {
  return kmem.total;
}
