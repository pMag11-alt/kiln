// Process-related system calls.
#include "defs.h"

i64 sys_fork(void) {
  return proc_fork();
}

i64 sys_exit(void) {
  proc_exit(((int)argraw(0) & 0xff) << 8);
}

i64 sys_waitpid(void) {
  return proc_wait((int)argraw(0), argraw(1), (int)argraw(2));
}

i64 sys_kill(void) {
  return proc_kill((int)argraw(0), (int)argraw(1));
}

i64 sys_getpid(void) {
  return myproc()->pid;
}

i64 sys_getppid(void) {
  acquire(&wait_lock);
  struct proc *parent = myproc()->parent;
  int ppid = parent ? parent->pid : 0;
  release(&wait_lock);
  return ppid;
}

// The heap is demand-paged: growing it only moves the break, and pages are
// allocated on first touch. Shrinking frees whatever was touched.
i64 sys_sbrk(void) {
  struct proc *p = myproc();
  i64 n = (i64)argraw(0);
  u64 old = p->brk;
  u64 new = old + (u64)n;
  if (n > 0) {
    if (new < old || new > HEAP_LIMIT) return -ENOMEM;
  } else if (n < 0) {
    if (new > old || new < p->heap_base) return -EINVAL;
    uvm_unmap_range(p->pagetable, ROUNDUP(new, PGSIZE), ROUNDUP(old, PGSIZE));
    sfence_vma();
  }
  p->brk = new;
  return (i64)old;
}

i64 sys_msleep(void) {
  u64 ms = argraw(0);
  u64 nticks = (ms * TICK_HZ + 999) / 1000;
  acquire(&tickslock);
  u64 start = ticks;
  while (ticks - start < nticks) {
    if (killed(myproc())) {
      release(&tickslock);
      return -EINTR;
    }
    sleep((void *)&ticks, &tickslock);
  }
  release(&tickslock);
  return 0;
}

i64 sys_uptime(void) {
  return (i64)uptime_ms();
}

i64 sys_time(void) {
  return (i64)rtc_now();
}

i64 sys_procinfo(void) {
  u64 dst = argraw(0);
  int max = (int)argraw(1);
  if (max <= 0) return -EINVAL;
  if (max > NPROC) max = NPROC;
  _Static_assert(NPROC * sizeof(struct procinfo) <= PGSIZE, "procinfo buffer");
  struct procinfo *buf = kalloc();
  if (!buf) return -ENOMEM;
  int n = proc_snapshot(buf, max);
  int r = copyout(myproc(), dst, buf, (u64)n * sizeof(*buf));
  kfree(buf);
  return r < 0 ? r : n;
}

i64 sys_sysinfo(void) {
  struct sysinfo si;
  memset(&si, 0, sizeof(si));
  si.uptime_ms = uptime_ms();
  si.total_pages = kmem_total_pages();
  si.free_pages = kmem_free_pages();
  si.nproc = (u32)proc_count();
  for (int i = 0; i < NCPU; i++) si.ncpu += cpus[i].online;
  si.syscalls = kstats.syscalls;
  si.ctx_switches = kstats.ctx_switches;
  si.cow_faults = kstats.cow_faults;
  si.lazy_faults = kstats.lazy_faults;
  si.disk_reads = kstats.disk_reads;
  si.disk_writes = kstats.disk_writes;
  si.bcache_hits = kstats.bcache_hits;
  si.bcache_misses = kstats.bcache_misses;
  fs_statfs(&si.fs_blocks, &si.fs_free_blocks, &si.fs_block_size, &si.fs_inodes, &si.fs_free_inodes);
  return copyout(myproc(), argraw(0), &si, sizeof(si));
}

i64 sys_shutdown(void) {
  int how = (int)argraw(0);
  kprintf("kiln: %s\n", how == SHUTDOWN_REBOOT ? "rebooting" : "powering off");
  // The buffer cache is write-through, so there is nothing to flush.
  sbi_system_reset(how == SHUTDOWN_REBOOT ? 1 : 0, 0);
}

i64 sys_setpgid(void) {
  return proc_setpgid((int)argraw(0), (int)argraw(1));
}

i64 sys_getpgid(void) {
  return proc_getpgid((int)argraw(0));
}

i64 sys_tcsetpgrp(void) {
  console_set_fg((int)argraw(0));
  return 0;
}
