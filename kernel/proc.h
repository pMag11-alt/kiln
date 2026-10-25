#pragma once

#include "param.h"
#include "types.h"

struct spinlock {
  volatile u32 locked;
  const char *name;
  struct cpu *cpu;    // holder, for debugging and holding()
};

// A lock that may be held across sleeps (disk I/O, pipe waits).
struct sleeplock {
  bool locked;
  struct spinlock lk;
  const char *name;
  int pid;
};

// Callee-saved registers preserved across swtch().
struct context {
  u64 ra, sp;
  u64 s0, s1, s2, s3, s4, s5, s6, s7, s8, s9, s10, s11;
};

struct cpu {
  struct proc *proc;        // process running on this hart, or NULL
  struct context context;   // swtch() here to enter the scheduler
  int noff;                 // depth of push_off() nesting
  bool intena;              // were interrupts enabled before push_off()?
  u64 hartid;
  int id;                   // index into cpus[]
  bool online;
};

extern struct cpu cpus[NCPU];

// User registers saved on a trap. The layout is fixed: trap.S indexes it.
// x1..x31 first (offset (n-1)*8), then the trap PC and the kernel values
// the trap vector needs to switch onto this process's kernel stack.
struct trapframe {
  u64 ra, sp, gp, tp, t0, t1, t2, s0, s1;
  u64 a0, a1, a2, a3, a4, a5, a6, a7;
  u64 s2, s3, s4, s5, s6, s7, s8, s9, s10, s11;
  u64 t3, t4, t5, t6;
  u64 epc;         // 248
  u64 kernel_sp;   // 256
  u64 kernel_tp;   // 264: struct cpu * of the hart that will take the trap
};

enum procstate { UNUSED, USED, SLEEPING, RUNNABLE, RUNNING, ZOMBIE };

struct proc {
  struct spinlock lock;

  // Protected by p->lock.
  enum procstate state;
  void *chan;           // sleeping on chan
  int killed;           // pending terminating signal, 0 if none
  int xstate;           // wait status for the parent
  int pid;
  int pgid;
  int cpu;              // cpus[] index while RUNNING, else -1

  // Protected by wait_lock.
  struct proc *parent;

  // Private to the process (or to whoever holds it during fork/exit).
  int slot;
  u64 kstack;           // bottom of kernel stack
  pagetable_t pagetable;
  u64 heap_base;        // start of the sbrk() heap
  u64 brk;              // current program break
  struct trapframe tf;
  struct context context;
  struct file *ofile[NOFILE];
  struct inode *cwd;
  char name[16];
  u64 ticks;
  bool first;           // the initial process mounts / and execs /bin/init
};

extern struct proc proc[NPROC];
