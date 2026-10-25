// execve(): replace the calling process's image with an ELF executable.
#include "defs.h"

#define ELF_MAGIC 0x464C457FU   // "\x7FELF" little-endian
#define ELFCLASS64 2
#define ET_EXEC 2
#define EM_RISCV 243
#define PT_LOAD 1
#define PF_X 1
#define PF_W 2
#define PF_R 4

struct elfhdr {
  u32 magic;
  u8 ident[12];
  u16 type;
  u16 machine;
  u32 version;
  u64 entry;
  u64 phoff;
  u64 shoff;
  u32 flags;
  u16 ehsize;
  u16 phentsize;
  u16 phnum;
  u16 shentsize;
  u16 shnum;
  u16 shstrndx;
};

struct proghdr {
  u32 type;
  u32 flags;
  u64 off;
  u64 vaddr;
  u64 paddr;
  u64 filesz;
  u64 memsz;
  u64 align;
};

// Map and fill one PT_LOAD segment. Bytes past filesz stay zero (bss).
static int load_segment(pagetable_t pt, struct inode *ip, const struct proghdr *ph, u64 perm) {
  u64 start = ROUNDDOWN(ph->vaddr, PGSIZE);
  u64 stop = ROUNDUP(ph->vaddr + ph->memsz, PGSIZE);
  for (u64 va = start; va < stop; va += PGSIZE) {
    pte_t *pte = walk(pt, va, false);
    char *kva;
    if (pte && (*pte & PTE_V)) {
      // A page shared with the previous segment: merge permissions.
      if (((*pte | perm) & (PTE_W | PTE_X)) == (PTE_W | PTE_X)) return -ENOEXEC;
      *pte |= perm;
      kva = P2V(PTE2PA(*pte));
    } else {
      kva = kzalloc();
      if (!kva) return -ENOMEM;
      if (uvm_map_page(pt, va, kva, perm) < 0) {
        kfree(kva);
        return -ENOMEM;
      }
    }
    u64 lo = MAX(va, ph->vaddr);
    u64 hi = MIN(va + PGSIZE, ph->vaddr + ph->filesz);
    if (hi > lo) {
      u64 n = hi - lo;
      if (readi(ip, false, (u64)(kva + (lo - va)), ph->off + (lo - ph->vaddr), n) != (i64)n)
        return -ENOEXEC;
    }
  }
  return 0;
}

int kexec(const char *path, char *const argv[]) {
  struct proc *p = myproc();
  struct inode *ip;
  pagetable_t pt = NULL;
  int r = namei(path, &ip);
  if (r < 0) return r;
  ilock(ip);

  struct elfhdr eh;
  r = -ENOEXEC;
  if (!S_ISREG(ip->d.i_mode)) {
    r = -EACCES;
    goto bad;
  }
  if (readi(ip, false, (u64)&eh, 0, sizeof(eh)) != sizeof(eh)) goto bad;
  if (eh.magic != ELF_MAGIC || eh.ident[0] != ELFCLASS64 || eh.type != ET_EXEC ||
      eh.machine != EM_RISCV || eh.phentsize != sizeof(struct proghdr))
    goto bad;

  pt = uvmcreate();
  if (!pt) {
    r = -ENOMEM;
    goto bad;
  }

  u64 top = USER_BASE;
  for (int i = 0; i < eh.phnum; i++) {
    struct proghdr ph;
    if (readi(ip, false, (u64)&ph, eh.phoff + (u64)i * sizeof(ph), sizeof(ph)) != sizeof(ph)) {
      r = -ENOEXEC;
      goto bad;
    }
    if (ph.type != PT_LOAD || ph.memsz == 0) continue;
    r = -ENOEXEC;
    if (ph.memsz < ph.filesz || ph.vaddr + ph.memsz < ph.vaddr) goto bad;
    if (ph.vaddr < USER_BASE || ph.vaddr + ph.memsz > HEAP_LIMIT) goto bad;
    if ((ph.vaddr - ph.off) % PGSIZE) goto bad;
    if ((ph.flags & PF_W) && (ph.flags & PF_X)) goto bad;   // W^X
    u64 perm = PTE_U | ((ph.flags & PF_R) ? PTE_R : 0) | ((ph.flags & PF_W) ? PTE_W : 0) |
               ((ph.flags & PF_X) ? PTE_X : 0);
    r = load_segment(pt, ip, &ph, perm);
    if (r < 0) goto bad;
    top = MAX(top, ph.vaddr + ph.memsz);
  }
  iunlockput(ip);
  ip = NULL;

  // The initial stack page holds the argument strings and argv[].
  void *stack = kzalloc();
  if (!stack) {
    r = -ENOMEM;
    goto bad;
  }
  if (uvm_map_page(pt, USTACK_TOP - PGSIZE, stack, PTE_U | PTE_R | PTE_W) < 0) {
    kfree(stack);
    r = -ENOMEM;
    goto bad;
  }
  u64 sp = USTACK_TOP;
  u64 uargv[MAXARG + 1];
  int argc = 0;
  r = -E2BIG;
  for (; argv[argc]; argc++) {
    if (argc >= MAXARG) goto bad;
    u64 len = strlen(argv[argc]) + 1;
    if (sp - len < USTACK_TOP - PGSIZE + (MAXARG + 2) * 8) goto bad;
    sp -= len;
    if (copyout_pt(pt, sp, argv[argc], len) < 0) goto bad;
    uargv[argc] = sp;
  }
  uargv[argc] = 0;
  sp -= (u64)(argc + 1) * 8;
  sp &= ~15UL;   // the ABI wants a 16-byte aligned stack
  if (copyout_pt(pt, sp, uargv, (u64)(argc + 1) * 8) < 0) goto bad;

  // Point of no return: switch to the new image.
  const char *last = path;
  for (const char *s = path; *s; s++)
    if (*s == '/') last = s + 1;
  strlcpy(p->name, last, sizeof(p->name));

  memset(&p->tf, 0, offsetof(struct trapframe, epc));
  p->tf.epc = eh.entry;
  p->tf.sp = sp;
  p->tf.a1 = sp;   // argv; a0 (argc) is the syscall's return value

  pagetable_t old = p->pagetable;
  push_off();
  p->pagetable = pt;
  p->heap_base = p->brk = ROUNDUP(top, PGSIZE);
  csr_write(satp, MAKE_SATP(V2P(pt)));
  sfence_vma();
  pop_off();
  uvmfree(old);
  return argc;

bad:
  if (pt) uvmfree(pt);
  if (ip) iunlockput(ip);
  return r;
}

i64 sys_execve(void) {
  struct proc *p = myproc();
  char path[MAXPATH];
  int r = argstr(0, path, sizeof(path));
  if (r < 0) return r;

  u64 uargv = argraw(1);
  char *argv[MAXARG + 1];
  memset(argv, 0, sizeof(argv));
  int i;
  for (i = 0;; i++) {
    if (i >= MAXARG) {
      r = -E2BIG;
      goto out;
    }
    u64 uarg;
    if (copyin(p, &uarg, uargv + 8 * (u64)i, 8) < 0) {
      r = -EFAULT;
      goto out;
    }
    if (!uarg) break;
    argv[i] = kalloc();
    if (!argv[i]) {
      r = -ENOMEM;
      goto out;
    }
    int n = copyinstr(p, argv[i], uarg, PGSIZE);
    if (n < 0) {
      r = n == -ENAMETOOLONG ? -E2BIG : n;
      goto out;
    }
  }
  argv[i] = NULL;
  r = kexec(path, argv);

out:
  for (int j = 0; j < MAXARG && argv[j]; j++) kfree(argv[j]);
  return r;
}
