// Open-file table and the character devices behind /dev/null and /dev/zero.
#include "defs.h"

struct devsw devsw[256];

static struct {
  struct spinlock lock;
  struct file file[NFILE];
} ftable;

static i64 mem_read(int minor, u64 udst, u64 n) {
  if (minor != MINOR_ZERO) return 0;   // /dev/null reads as EOF
  static const u8 zero_page[PGSIZE];
  for (u64 done = 0; done < n;) {
    u64 m = MIN(n - done, (u64)PGSIZE);
    if (copyout(myproc(), udst + done, zero_page, m) < 0) return done ? (i64)done : -EFAULT;
    done += m;
  }
  return (i64)n;
}

static i64 mem_write(int minor, u64 usrc, u64 n) {
  (void)minor;
  (void)usrc;
  return (i64)n;   // discarded
}

void fileinit(void) {
  initlock(&ftable.lock, "ftable");
  for (int i = 0; i < NFILE; i++) initsleeplock(&ftable.file[i].poslock, "file");
  devsw[MAJOR_MEM].read = mem_read;
  devsw[MAJOR_MEM].write = mem_write;
}

struct file *filealloc(void) {
  acquire(&ftable.lock);
  for (struct file *f = ftable.file; f < ftable.file + NFILE; f++) {
    if (f->ref == 0) {
      f->ref = 1;
      f->type = FD_NONE;
      f->readable = f->writable = f->append = false;
      f->pipe = NULL;
      f->ip = NULL;
      f->off = 0;
      f->rdev = 0;
      release(&ftable.lock);
      return f;
    }
  }
  release(&ftable.lock);
  return NULL;
}

struct file *filedup(struct file *f) {
  acquire(&ftable.lock);
  if (f->ref < 1) panic("filedup: closed file");
  f->ref++;
  release(&ftable.lock);
  return f;
}

void fileclose(struct file *f) {
  acquire(&ftable.lock);
  if (f->ref < 1) panic("fileclose: closed file");
  if (--f->ref > 0) {
    release(&ftable.lock);
    return;
  }
  enum ftype type = f->type;
  struct pipe *pipe = f->pipe;
  struct inode *ip = f->ip;
  bool writable = f->writable;
  f->type = FD_NONE;
  release(&ftable.lock);

  if (type == FD_PIPE) pipeclose(pipe, writable);
  else if (type == FD_INODE || type == FD_DEVICE) iput(ip);
}

int filestat(struct file *f, u64 dst) {
  struct stat st;
  if (f->type == FD_PIPE) {
    memset(&st, 0, sizeof(st));
    st.st_mode = S_IFIFO | 0600;
    st.st_nlink = 1;
  } else if (f->type == FD_INODE || f->type == FD_DEVICE) {
    ilock(f->ip);
    stati(f->ip, &st);
    iunlock(f->ip);
  } else {
    return -EBADF;
  }
  return copyout(myproc(), dst, &st, sizeof(st));
}

i64 fileread(struct file *f, u64 dst, u64 n) {
  if (!f->readable) return -EBADF;
  switch (f->type) {
  case FD_PIPE:
    return piperead(f->pipe, dst, n);
  case FD_DEVICE: {
    struct devsw *d = &devsw[DEV_MAJOR(f->rdev)];
    return d->read ? d->read(DEV_MINOR(f->rdev), dst, n) : -ENODEV;
  }
  case FD_INODE: {
    acquiresleep(&f->poslock);
    ilock(f->ip);
    i64 r = S_ISDIR(f->ip->d.i_mode) ? -EISDIR : readi(f->ip, true, dst, f->off, n);
    if (r > 0) f->off += (u64)r;
    iunlock(f->ip);
    releasesleep(&f->poslock);
    return r;
  }
  default:
    panic("fileread: bad file type");
  }
}

i64 filewrite(struct file *f, u64 src, u64 n) {
  if (!f->writable) return -EBADF;
  switch (f->type) {
  case FD_PIPE:
    return pipewrite(f->pipe, src, n);
  case FD_DEVICE: {
    struct devsw *d = &devsw[DEV_MAJOR(f->rdev)];
    return d->write ? d->write(DEV_MINOR(f->rdev), src, n) : -ENODEV;
  }
  case FD_INODE: {
    acquiresleep(&f->poslock);
    ilock(f->ip);
    if (f->append) f->off = f->ip->d.i_size;
    i64 r = writei(f->ip, true, src, f->off, n);
    if (r > 0) f->off += (u64)r;
    iunlock(f->ip);
    releasesleep(&f->poslock);
    return r;
  }
  default:
    panic("filewrite: bad file type");
  }
}
