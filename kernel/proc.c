// Processes and the scheduler.
#include "defs.h"

struct cpu cpus[NCPU];
struct proc proc[NPROC];

static struct proc *init_proc;
static int nextpid = 1;
static struct spinlock pid_lock;

// Guards p->parent for every process and orders wakeups of waiting parents
// against children exiting. Always acquired before any p->lock.
struct spinlock wait_lock;

extern void forkret(void);

void procinit(void) {
  initlock(&pid_lock, "nextpid");
  initlock(&wait_lock, "wait_lock");
  for (int i = 0; i < NPROC; i++) {
    struct proc *p = &proc[i];
    initlock(&p->lock, "proc");
    p->state = UNUSED;
    p->slot = i;
    p->cpu = -1;
    p->kstack = KSTACK(i);
  }
}

// The hart's struct cpu lives in tp. Callers that use the result for more
// than a moment must have interrupts off, or they might migrate.
struct cpu *mycpu(void) {
  return (struct cpu *)r_tp();
}

struct proc *myproc(void) {
  push_off();
  struct proc *p = mycpu()->proc;
  pop_off();
  return p;
}

static int allocpid(void) {
  acquire(&pid_lock);
  int pid = nextpid++;
  release(&pid_lock);
  return pid;
}

// Claim an UNUSED slot. Returns with p->lock held.
static struct proc *allocproc(void) {
  struct proc *p;
  for (p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if (p->state == UNUSED) goto found;
    release(&p->lock);
  }
  return NULL;

found:
  p->pid = allocpid();
  p->pgid = p->pid;
  p->state = USED;
  p->killed = 0;
  p->xstate = 0;
  p->ticks = 0;
  p->cpu = -1;
  p->first = false;
  p->pagetable = uvmcreate();
  if (!p->pagetable) {
    p->state = UNUSED;
    release(&p->lock);
    return NULL;
  }
  memset(&p->tf, 0, sizeof(p->tf));
  memset(&p->context, 0, sizeof(p->context));
  // A new process starts life in forkret(), on its own kernel stack.
  p->context.ra = (u64)forkret;
  p->context.sp = p->kstack + KSTACK_SIZE;
  return p;
}

// Release a slot. p->lock must be held.
static void freeproc(struct proc *p) {
  if (p->pagetable) uvmfree(p->pagetable);
  p->pagetable = NULL;
  p->heap_base = p->brk = 0;
  p->pid = 0;
  p->pgid = 0;
  p->parent = NULL;
  p->name[0] = '\0';
  p->chan = NULL;
  p->killed = 0;
  p->xstate = 0;
  p->state = UNUSED;
}

// Create the first process. It runs in the kernel until forkret() mounts
// the root file system and execs /bin/init.
void userinit(void) {
  struct proc *p = allocproc();
  if (!p) panic("userinit: no process slot");
  init_proc = p;
  p->first = true;
  strlcpy(p->name, "init", sizeof(p->name));
  p->state = RUNNABLE;
  release(&p->lock);
}

int proc_fork(void) {
  struct proc *p = myproc();
  struct proc *np = allocproc();
  if (!np) return -EAGAIN;

  if (uvmcopy(p->pagetable, np->pagetable) < 0) {
    freeproc(np);
    release(&np->lock);
    return -ENOMEM;
  }
  np->heap_base = p->heap_base;
  np->brk = p->brk;
  np->tf = p->tf;
  np->tf.a0 = 0;   // fork() returns 0 in the child

  for (int i = 0; i < NOFILE; i++)
    if (p->ofile[i]) np->ofile[i] = filedup(p->ofile[i]);
  np->cwd = idup(p->cwd);
  memcpy(np->name, p->name, sizeof(p->name));
  np->pgid = p->pgid;
  int pid = np->pid;
  release(&np->lock);

  acquire(&wait_lock);
  np->parent = p;
  release(&wait_lock);

  acquire(&np->lock);
  np->state = RUNNABLE;
  release(&np->lock);
  return pid;
}

// Hand p's children to init. Caller holds wait_lock.
static void reparent(struct proc *p) {
  for (struct proc *pp = proc; pp < &proc[NPROC]; pp++) {
    if (pp->parent == p) {
      pp->parent = init_proc;
      wakeup(init_proc);
    }
  }
}

void proc_exit(int wstatus) {
  struct proc *p = myproc();
  if (p == init_proc) panic("init exited (status %#x)", wstatus);

  for (int fd = 0; fd < NOFILE; fd++) {
    if (p->ofile[fd]) {
      fileclose(p->ofile[fd]);
      p->ofile[fd] = NULL;
    }
  }
  if (p->cwd) iput(p->cwd);
  p->cwd = NULL;

  // Give the memory back now rather than when the parent reaps us. The
  // kernel half is identical in every table, so switching is seamless.
  push_off();
  csr_write(satp, MAKE_SATP(V2P(kernel_pagetable)));
  sfence_vma();
  pagetable_t pt = p->pagetable;
  p->pagetable = NULL;
  pop_off();
  uvmfree(pt);

  acquire(&wait_lock);
  reparent(p);
  wakeup(p->parent);
  acquire(&p->lock);
  p->xstate = wstatus;
  p->state = ZOMBIE;
  release(&wait_lock);

  sched();
  panic("zombie exit");
}

// waitpid(): pid -1 waits for any child.
int proc_wait(int pid, u64 status_uaddr, int options) {
  struct proc *p = myproc();
  acquire(&wait_lock);
  for (;;) {
    bool have_kids = false;
    for (struct proc *pp = proc; pp < &proc[NPROC]; pp++) {
      if (pp->parent != p || (pid > 0 && pp->pid != pid)) continue;
      acquire(&pp->lock);
      have_kids = true;
      if (pp->state == ZOMBIE) {
        int cpid = pp->pid;
        int xs = pp->xstate;
        freeproc(pp);
        release(&pp->lock);
        release(&wait_lock);
        if (status_uaddr && copyout(p, status_uaddr, &xs, sizeof(xs)) < 0) return -EFAULT;
        return cpid;
      }
      release(&pp->lock);
    }
    if (!have_kids) {
      release(&wait_lock);
      return -ECHILD;
    }
    if (options & WNOHANG) {
      release(&wait_lock);
      return 0;
    }
    if (killed(p)) {
      release(&wait_lock);
      return -EINTR;
    }
    sleep(p, &wait_lock);
  }
}

// Deliver a terminating signal. pid < 0 targets the process group -pid.
int proc_kill(int pid, int sig) {
  if (sig <= 0 || sig >= NSIG) return -EINVAL;
  int hits = 0;
  for (struct proc *p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    bool match = p->state != UNUSED && p->state != ZOMBIE &&
                 (pid > 0 ? p->pid == pid : p->pgid == -pid);
    if (match && p != init_proc) {
      if (!p->killed) p->killed = sig;
      if (p->state == SLEEPING) p->state = RUNNABLE;
      hits++;
    }
    release(&p->lock);
  }
  return hits ? 0 : -ESRCH;
}

void setkilled(struct proc *p, int sig) {
  acquire(&p->lock);
  if (!p->killed) p->killed = sig;
  release(&p->lock);
}

int killed(struct proc *p) {
  acquire(&p->lock);
  int k = p->killed;
  release(&p->lock);
  return k;
}

int proc_setpgid(int pid, int pgid) {
  struct proc *me = myproc();
  if (pid == 0) pid = me->pid;
  if (pgid == 0) pgid = pid;
  if (pgid < 0) return -EINVAL;
  int r = -ESRCH;
  acquire(&wait_lock);
  for (struct proc *p = proc; p < &proc[NPROC]; p++) {
    if (p->pid != pid || p->state == UNUSED) continue;
    if (p != me && p->parent != me) { r = -EPERM; break; }
    acquire(&p->lock);
    p->pgid = pgid;
    release(&p->lock);
    r = 0;
    break;
  }
  release(&wait_lock);
  return r;
}

int proc_getpgid(int pid) {
  if (pid == 0) return myproc()->pgid;
  for (struct proc *p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if (p->pid == pid && p->state != UNUSED) {
      int g = p->pgid;
      release(&p->lock);
      return g;
    }
    release(&p->lock);
  }
  return -ESRCH;
}

int proc_count(void) {
  int n = 0;
  for (struct proc *p = proc; p < &proc[NPROC]; p++)
    if (__atomic_load_n(&p->state, __ATOMIC_RELAXED) != UNUSED) n++;
  return n;
}

int proc_snapshot(struct procinfo *out, int max) {
  int n = 0;
  acquire(&wait_lock);
  for (struct proc *p = proc; p < &proc[NPROC] && n < max; p++) {
    acquire(&p->lock);
    if (p->state == UNUSED || p->state == USED) {
      release(&p->lock);
      continue;
    }
    struct procinfo *pi = &out[n++];
    memset(pi, 0, sizeof(*pi));
    pi->pid = p->pid;
    pi->ppid = p->parent ? p->parent->pid : 0;
    pi->pgid = p->pgid;
    pi->state = p->state == SLEEPING ? PS_SLEEPING
              : p->state == RUNNABLE ? PS_RUNNABLE
              : p->state == RUNNING ? PS_RUNNING : PS_ZOMBIE;
    pi->cpu = p->state == RUNNING ? p->cpu : -1;
    pi->pages = uvm_count_pages(p->pagetable);
    pi->ticks = p->ticks;
    memcpy(pi->name, p->name, sizeof(pi->name));
    release(&p->lock);
  }
  release(&wait_lock);
  return n;
}

// Per-hart scheduler loop: pick RUNNABLE processes round-robin. The hart
// runs a process with that process's page table loaded and switches back
// to the kernel table afterwards, so a freed table is never live.
void scheduler(void) {
  struct cpu *c = mycpu();
  c->proc = NULL;
  for (;;) {
    // Let devices interrupt us: a quiet system would otherwise deadlock
    // with every process waiting on I/O.
    intr_on();
    bool ran = false;
    for (struct proc *p = proc; p < &proc[NPROC]; p++) {
      acquire(&p->lock);
      if (p->state == RUNNABLE) {
        p->state = RUNNING;
        p->cpu = c->id;
        c->proc = p;
        // An exiting process has already dropped its user page table.
        pagetable_t pt = p->pagetable ? p->pagetable : kernel_pagetable;
        csr_write(satp, MAKE_SATP(V2P(pt)));
        sfence_vma();
        STAT_INC(ctx_switches);
        swtch(&c->context, &p->context);
        csr_write(satp, MAKE_SATP(V2P(kernel_pagetable)));
        sfence_vma();
        c->proc = NULL;
        ran = true;
      }
      release(&p->lock);
    }
    if (!ran) wfi();
  }
}

// Switch to the scheduler. The caller holds only p->lock and has already
// changed p->state.
void sched(void) {
  struct proc *p = myproc();
  if (!holding(&p->lock)) panic("sched: p->lock not held");
  if (mycpu()->noff != 1) panic("sched: holding extra locks");
  if (p->state == RUNNING) panic("sched: still running");
  if (intr_get()) panic("sched: interruptible");
  bool intena = mycpu()->intena;
  p->cpu = -1;
  swtch(&p->context, &mycpu()->context);
  mycpu()->intena = intena;
}

void yield(void) {
  struct proc *p = myproc();
  acquire(&p->lock);
  p->state = RUNNABLE;
  sched();
  release(&p->lock);
}

// Every new process starts here, still holding p->lock from scheduler().
void forkret(void) {
  struct proc *p = myproc();
  release(&p->lock);
  if (p->first) {
    p->first = false;
    fs_mount();
    if (namei("/", &p->cwd) < 0) panic("forkret: no root directory");
    char *argv[] = {"/bin/init", NULL};
    int r = kexec("/bin/init", argv);
    if (r < 0) panic("forkret: cannot exec /bin/init (error %d)", r);
    p->tf.a0 = (u64)r;
  }
  usertrapret();
}

// Atomically release lk and sleep on chan; reacquire lk when woken.
void sleep(void *chan, struct spinlock *lk) {
  struct proc *p = myproc();
  // Holding p->lock means no wakeup can slip between releasing lk and
  // marking ourselves SLEEPING: wakeup() needs p->lock too.
  acquire(&p->lock);
  release(lk);
  p->chan = chan;
  p->state = SLEEPING;
  sched();
  p->chan = NULL;
  release(&p->lock);
  acquire(lk);
}

void wakeup(void *chan) {
  struct proc *me = myproc();
  for (struct proc *p = proc; p < &proc[NPROC]; p++) {
    if (p == me) continue;
    acquire(&p->lock);
    if (p->state == SLEEPING && p->chan == chan) p->state = RUNNABLE;
    release(&p->lock);
  }
}

// Ctrl-P on the console: list every process and what it is waiting for.
// No locks, so it works even when the system is wedged.
void procdump(void) {
  static const char *names[] = {
    [UNUSED] = "unused", [USED] = "used", [SLEEPING] = "sleep",
    [RUNNABLE] = "runble", [RUNNING] = "run", [ZOMBIE] = "zombie",
  };
  kprintf("\n");
  for (struct proc *p = proc; p < &proc[NPROC]; p++) {
    if (p->state == UNUSED) continue;
    kprintf("%3d %-6s %-12s chan=%p cpu=%d killed=%d\n", p->pid, names[p->state], p->name,
            p->chan, p->cpu, p->killed);
  }
}
