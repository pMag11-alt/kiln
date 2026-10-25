// System call dispatch. Arguments arrive in a0-a5, the number in a7; the
// result (or a negative errno) goes back in a0.
#include "defs.h"

#define SYSCALLS(X) \
  X(fork) X(exit) X(waitpid) X(pipe) X(read) X(write) X(close) X(kill) \
  X(execve) X(fstat) X(chdir) X(dup) X(getpid) X(sbrk) X(msleep) X(uptime) \
  X(open) X(mknod) X(unlink) X(link) X(mkdir) X(getdents) X(lseek) X(dup2) \
  X(getppid) X(rename) X(rmdir) X(stat) X(getcwd) X(procinfo) X(sysinfo) \
  X(shutdown) X(setpgid) X(getpgid) X(tcsetpgrp) X(time) X(ftruncate)

#define DECLARE(name) i64 sys_##name(void);
SYSCALLS(DECLARE)

#define ENTRY(name) [SYS_##name] = sys_##name,
static i64 (*const syscalls[NSYSCALL])(void) = { SYSCALLS(ENTRY) };

void syscall(void) {
  struct proc *p = myproc();
  u64 num = p->tf.a7;
  STAT_INC(syscalls);
  i64 r = (num < NSYSCALL && syscalls[num]) ? syscalls[num]() : -ENOSYS;
  p->tf.a0 = (u64)r;
}

u64 argraw(int n) {
  struct trapframe *tf = &myproc()->tf;
  switch (n) {
  case 0: return tf->a0;
  case 1: return tf->a1;
  case 2: return tf->a2;
  case 3: return tf->a3;
  case 4: return tf->a4;
  case 5: return tf->a5;
  }
  panic("argraw: bad index %d", n);
}

int argstr(int n, char *buf, int max) {
  return copyinstr(myproc(), buf, argraw(n), (u64)max);
}
