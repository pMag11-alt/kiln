// Sv39 page tables: the kernel map, user address spaces, copy-on-write and
// demand paging, and safe copies between kernel and user memory.
#include "defs.h"

extern char kernel_start[], etext[], erodata[], end[];

pagetable_t kernel_pagetable;

#define MEGA (1UL << 21)

// Return the PTE for va at the given level, creating intermediate tables
// when alloc is set.
static pte_t *walk_level(pagetable_t pt, u64 va, int target, bool alloc) {
  for (int level = 2; level > target; level--) {
    pte_t *pte = &pt[PX(level, va)];
    if (*pte & PTE_V) {
      if (PTE_LEAF(*pte)) panic("walk: superpage on the path to %p", (void *)va);
      pt = P2V(PTE2PA(*pte));
    } else {
      if (!alloc) return NULL;
      pagetable_t next = kzalloc();
      if (!next) return NULL;
      *pte = PA2PTE(V2P(next)) | PTE_V;
      pt = next;
    }
  }
  return &pt[PX(target, va)];
}

pte_t *walk(pagetable_t pt, u64 va, bool alloc) {
  return walk_level(pt, va, 0, alloc);
}

// Map [va, va+size) to [pa, pa+size) in the kernel page table, using 2 MiB
// pages wherever alignment allows.
static void kmap(u64 va, u64 pa, u64 size, u64 perm) {
  u64 stop = va + size;
  while (va < stop) {
    bool mega = (va % MEGA) == 0 && (pa % MEGA) == 0 && stop - va >= MEGA;
    pte_t *pte = walk_level(kernel_pagetable, va, mega ? 1 : 0, true);
    if (!pte) panic("kmap: out of memory");
    if (*pte & PTE_V) panic("kmap: %p already mapped", (void *)va);
    *pte = PA2PTE(pa) | perm | PTE_V | PTE_A | PTE_D | PTE_G;
    u64 step = mega ? MEGA : PGSIZE;
    va += step;
    pa += step;
  }
}

static void kmap_device(u64 pa, u64 size) {
  if (pa) kmap((u64)P2V(pa), pa, ROUNDUP(size, PGSIZE), PTE_R | PTE_W);
}

void kvminit(void) {
  kernel_pagetable = kzalloc();
  if (!kernel_pagetable) panic("kvminit: out of memory");

  // The kernel image with W^X permissions.
  kmap((u64)kernel_start, V2P(kernel_start), etext - kernel_start, PTE_R | PTE_X);
  kmap((u64)etext, V2P(etext), erodata - etext, PTE_R);
  kmap((u64)erodata, V2P(erodata), end - erodata, PTE_R | PTE_W);

  // Direct map of the rest of RAM: page tables, user pages, the heap.
  u64 ram_end = plat.ram_base + plat.ram_size;
  kmap((u64)end, V2P(end), ram_end - V2P(end), PTE_R | PTE_W);

  kmap_device(plat.plic_base, plat.plic_size);
  kmap_device(plat.uart_base, PGSIZE);
  kmap_device(plat.rtc_base, PGSIZE);
  for (int i = 0; i < plat.nvirtio; i++) kmap_device(plat.virtio_base[i], PGSIZE);

  // One kernel stack per process slot, each with an unmapped guard page
  // below it so an overflow faults instead of corrupting memory.
  for (int i = 0; i < NPROC; i++) {
    for (int j = 0; j < KSTACK_PAGES; j++) {
      void *pg = kzalloc();
      if (!pg) panic("kvminit: no memory for kernel stacks");
      kmap(KSTACK(i) + (u64)j * PGSIZE, V2P(pg), PGSIZE, PTE_R | PTE_W);
    }
  }
}

void kvminithart(void) {
  sfence_vma();
  csr_write(satp, MAKE_SATP(V2P(kernel_pagetable)));
  sfence_vma();
}

// A new user page table shares the kernel's upper-half tables.
pagetable_t uvmcreate(void) {
  pagetable_t pt = kzalloc();
  if (!pt) return NULL;
  memcpy(&pt[256], &kernel_pagetable[256], 256 * sizeof(pte_t));
  return pt;
}

static void free_table(pagetable_t pt, int level) {
  for (int i = 0; i < 512; i++) {
    pte_t pte = pt[i];
    if (!(pte & PTE_V)) continue;
    if (PTE_LEAF(pte)) {
      if (level != 0) panic("free_table: user superpage");
      kfree(P2V(PTE2PA(pte)));
    } else {
      free_table(P2V(PTE2PA(pte)), level - 1);
    }
  }
  kfree(pt);
}

// Free a user page table, every user page it maps (dropping one reference
// each) and its intermediate tables. The shared kernel half is untouched.
void uvmfree(pagetable_t pt) {
  for (int i = 0; i < 256; i++) {
    pte_t pte = pt[i];
    if (!(pte & PTE_V)) continue;
    if (PTE_LEAF(pte)) panic("uvmfree: user gigapage");
    free_table(P2V(PTE2PA(pte)), 1);
  }
  kfree(pt);
}

static int copy_table(pagetable_t old, pagetable_t new, int level, int count) {
  for (int i = 0; i < count; i++) {
    pte_t *op = &old[i];
    if (!(*op & PTE_V)) continue;
    if (level == 0) {
      // Share the page. Writable pages become read-only + COW in both
      // address spaces; the first write to either side takes a fault.
      u64 flags = PTE_FLAGS(*op);
      if (flags & PTE_W) {
        flags = (flags & ~PTE_W) | PTE_COW;
        *op = PA2PTE(PTE2PA(*op)) | flags;
      }
      kref_inc(P2V(PTE2PA(*op)));
      new[i] = *op;
    } else {
      pagetable_t next = kzalloc();
      if (!next) return -ENOMEM;
      new[i] = PA2PTE(V2P(next)) | PTE_V;
      int r = copy_table(P2V(PTE2PA(*op)), next, level - 1, 512);
      if (r < 0) return r;
    }
  }
  return 0;
}

// Copy-on-write duplicate of the user half of old into new. On failure the
// caller frees new; old stays valid (its pages are merely marked COW).
int uvmcopy(pagetable_t old, pagetable_t new) {
  int r = copy_table(old, new, 2, 256);
  sfence_vma();   // old lost write permission on shared pages
  return r;
}

int uvm_map_page(pagetable_t pt, u64 va, void *kva, u64 perm) {
  pte_t *pte = walk(pt, va, true);
  if (!pte) return -ENOMEM;
  if (*pte & PTE_V) panic("uvm_map_page: %p already mapped", (void *)va);
  *pte = PA2PTE(V2P(kva)) | perm | PTE_V | PTE_A | PTE_D;
  return 0;
}

// Unmap and release every page in [start, end). Holes are fine (lazy pages
// that were never touched). Leaf page tables left empty are freed too. The
// caller flushes the TLB.
void uvm_unmap_range(pagetable_t pt, u64 start, u64 stop) {
  for (u64 va = ROUNDDOWN(start, PGSIZE); va < stop;) {
    u64 next = ROUNDDOWN(va, MEGA) + MEGA;   // the end of this leaf table
    pte_t *l1 = walk_level(pt, va, 1, false);
    if (!l1 || !(*l1 & PTE_V)) {
      va = next;
      continue;
    }
    pagetable_t leaf = P2V(PTE2PA(*l1));
    for (; va < next && va < stop; va += PGSIZE) {
      pte_t *pte = &leaf[PX(0, va)];
      if (*pte & PTE_V) {
        kfree(P2V(PTE2PA(*pte)));
        *pte = 0;
      }
    }
    bool empty = true;
    for (int i = 0; i < 512 && empty; i++) empty = !(leaf[i] & PTE_V);
    if (empty) {
      *l1 = 0;
      kfree(leaf);
    }
  }
}

static u32 count_table(pagetable_t pt, int level, int count) {
  u32 n = 0;
  for (int i = 0; i < count; i++) {
    if (!(pt[i] & PTE_V)) continue;
    n += level == 0 ? 1 : count_table(P2V(PTE2PA(pt[i])), level - 1, 512);
  }
  return n;
}

u32 uvm_count_pages(pagetable_t pt) {
  return pt ? count_table(pt, 2, 256) : 0;
}

// ---- page faults ----

static bool lazy_region(struct proc *p, u64 va) {
  return (va >= p->heap_base && va < p->brk) ||
         (va >= USTACK_BOTTOM && va < USTACK_TOP);
}

// Back an untouched heap or stack page with a fresh zero page.
static int lazy_fault(struct proc *p, u64 va) {
  if (!lazy_region(p, va)) return -EFAULT;
  void *pg = kzalloc();
  if (!pg) return -ENOMEM;
  if (uvm_map_page(p->pagetable, ROUNDDOWN(va, PGSIZE), pg, PTE_U | PTE_R | PTE_W) < 0) {
    kfree(pg);
    return -ENOMEM;
  }
  sfence_vma_addr(ROUNDDOWN(va, PGSIZE));
  STAT_INC(lazy_faults);
  return 0;
}

// Give the faulting process a private, writable copy of a COW page. If it
// is the last sharer, the page is simply made writable again.
static int cow_fault(pagetable_t pt, u64 va) {
  pte_t *pte = walk(pt, va, false);
  if (!pte || !(*pte & PTE_V) || !(*pte & PTE_COW)) return -EFAULT;
  void *old = P2V(PTE2PA(*pte));
  u64 flags = (PTE_FLAGS(*pte) | PTE_W | PTE_D) & ~PTE_COW;
  if (kref_get(old) == 1) {
    *pte = PA2PTE(V2P(old)) | flags;
  } else {
    void *copy = kalloc();
    if (!copy) return -ENOMEM;
    memcpy(copy, old, PGSIZE);
    *pte = PA2PTE(V2P(copy)) | flags;
    kfree(old);
  }
  sfence_vma_addr(ROUNDDOWN(va, PGSIZE));
  STAT_INC(cow_faults);
  return 0;
}

// Resolve a user-mode page fault. Returns 0 if the access can be retried,
// -EFAULT for a genuine fault and -ENOMEM when memory ran out.
int handle_user_fault(struct proc *p, u64 va, u64 scause) {
  if (va >= USER_TOP) return -EFAULT;
  pte_t *pte = walk(p->pagetable, va, false);
  if (pte && (*pte & PTE_V)) {
    if (scause == EXC_STORE_PAGE_FAULT && (*pte & PTE_COW)) return cow_fault(p->pagetable, va);
    return -EFAULT;
  }
  return lazy_fault(p, va);
}

// ---- kernel <-> user copies ----

// Translate a user address for a kernel access, faulting in lazy and COW
// pages exactly as a user access would. p may be NULL when filling a page
// table that is not live yet (exec), in which case nothing is faulted in.
static char *user_kva(struct proc *p, pagetable_t pt, u64 va, bool write) {
  if (va >= USER_TOP) return NULL;
  pte_t *pte = walk(pt, va, false);
  if (!pte || !(*pte & PTE_V)) {
    if (!p || lazy_fault(p, va) < 0) return NULL;
    pte = walk(pt, va, false);
  }
  if (!(*pte & PTE_U)) return NULL;
  if (write && !(*pte & PTE_W)) {
    if (!p || !(*pte & PTE_COW) || cow_fault(pt, va) < 0) return NULL;
  }
  if (!write && !(*pte & PTE_R)) return NULL;
  return (char *)P2V(PTE2PA(*pte)) + (va & (PGSIZE - 1));
}

static int copy_out(struct proc *p, pagetable_t pt, u64 dst, const void *src, u64 len) {
  const char *s = src;
  while (len) {
    char *k = user_kva(p, pt, dst, true);
    if (!k) return -EFAULT;
    u64 n = MIN(len, PGSIZE - (dst & (PGSIZE - 1)));
    memcpy(k, s, n);
    len -= n;
    s += n;
    dst += n;
  }
  return 0;
}

int copyout(struct proc *p, u64 dst, const void *src, u64 len) {
  return copy_out(p, p->pagetable, dst, src, len);
}

int copyout_pt(pagetable_t pt, u64 dst, const void *src, u64 len) {
  return copy_out(NULL, pt, dst, src, len);
}

int copyin(struct proc *p, void *dst, u64 src, u64 len) {
  char *d = dst;
  while (len) {
    char *k = user_kva(p, p->pagetable, src, false);
    if (!k) return -EFAULT;
    u64 n = MIN(len, PGSIZE - (src & (PGSIZE - 1)));
    memcpy(d, k, n);
    len -= n;
    d += n;
    src += n;
  }
  return 0;
}

// Copy a NUL-terminated string. Returns its length, -EFAULT, or
// -ENAMETOOLONG if no NUL appears within max bytes.
int copyinstr(struct proc *p, char *dst, u64 src, u64 max) {
  u64 got = 0;
  while (got < max) {
    char *k = user_kva(p, p->pagetable, src, false);
    if (!k) return -EFAULT;
    u64 n = MIN(max - got, PGSIZE - (src & (PGSIZE - 1)));
    for (u64 i = 0; i < n; i++) {
      dst[got] = k[i];
      if (k[i] == '\0') return (int)got;
      got++;
    }
    src += n;
  }
  if (max) dst[max - 1] = '\0';
  return -ENAMETOOLONG;
}
