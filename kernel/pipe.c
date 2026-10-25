// Pipes: a ring buffer in one page, shared by a read end and a write end.
#include "defs.h"

struct pipe {
  struct spinlock lock;
  u64 nread;      // total bytes read
  u64 nwrite;     // total bytes written
  bool readopen;
  bool writeopen;
  char data[];
};

#define PIPE_CAP (PGSIZE - sizeof(struct pipe))

int pipealloc(struct file **f0, struct file **f1) {
  struct pipe *pi = kzalloc();
  *f0 = *f1 = NULL;
  if (!pi) return -ENOMEM;
  if (!(*f0 = filealloc()) || !(*f1 = filealloc())) {
    if (*f0) fileclose(*f0);
    kfree(pi);
    return -ENFILE;
  }
  initlock(&pi->lock, "pipe");
  pi->readopen = pi->writeopen = true;
  (*f0)->type = FD_PIPE;
  (*f0)->readable = true;
  (*f0)->pipe = pi;
  (*f1)->type = FD_PIPE;
  (*f1)->writable = true;
  (*f1)->pipe = pi;
  return 0;
}

void pipeclose(struct pipe *pi, bool writable) {
  acquire(&pi->lock);
  if (writable) {
    pi->writeopen = false;
    wakeup(&pi->nread);
  } else {
    pi->readopen = false;
    wakeup(&pi->nwrite);
  }
  bool dead = !pi->readopen && !pi->writeopen;
  release(&pi->lock);
  if (dead) kfree(pi);
}

i64 pipewrite(struct pipe *pi, u64 src, u64 n) {
  struct proc *p = myproc();
  u64 done = 0;
  acquire(&pi->lock);
  while (done < n) {
    if (!pi->readopen) {
      // Nobody will ever read this: SIGPIPE, like Unix.
      release(&pi->lock);
      setkilled(p, SIGPIPE);
      return done ? (i64)done : -EPIPE;
    }
    if (killed(p)) {
      release(&pi->lock);
      return done ? (i64)done : -EINTR;
    }
    u64 used = pi->nwrite - pi->nread;
    if (used == PIPE_CAP) {
      wakeup(&pi->nread);
      sleep(&pi->nwrite, &pi->lock);
      continue;
    }
    u64 off = pi->nwrite % PIPE_CAP;
    u64 m = MIN(MIN(n - done, PIPE_CAP - used), PIPE_CAP - off);
    if (copyin(p, &pi->data[off], src + done, m) < 0) {
      release(&pi->lock);
      return done ? (i64)done : -EFAULT;
    }
    pi->nwrite += m;
    done += m;
  }
  wakeup(&pi->nread);
  release(&pi->lock);
  return (i64)done;
}

i64 piperead(struct pipe *pi, u64 dst, u64 n) {
  struct proc *p = myproc();
  acquire(&pi->lock);
  while (pi->nread == pi->nwrite && pi->writeopen) {
    if (killed(p)) {
      release(&pi->lock);
      return -EINTR;
    }
    sleep(&pi->nread, &pi->lock);
  }
  u64 done = 0;
  while (done < n && pi->nread != pi->nwrite) {
    u64 off = pi->nread % PIPE_CAP;
    u64 m = MIN(MIN(n - done, pi->nwrite - pi->nread), PIPE_CAP - off);
    if (copyout(p, dst + done, &pi->data[off], m) < 0) {
      if (!done) {
        release(&pi->lock);
        return -EFAULT;
      }
      break;
    }
    pi->nread += m;
    done += m;
  }
  wakeup(&pi->nwrite);
  release(&pi->lock);
  return (i64)done;
}
