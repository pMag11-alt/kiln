// File-system system calls.
#include "defs.h"

static int argfd(int n, int *pfd, struct file **pf) {
  int fd = (int)argraw(n);
  if (fd < 0 || fd >= NOFILE) return -EBADF;
  struct file *f = myproc()->ofile[fd];
  if (!f) return -EBADF;
  if (pfd) *pfd = fd;
  if (pf) *pf = f;
  return 0;
}

static int fdalloc(struct file *f) {
  struct proc *p = myproc();
  for (int fd = 0; fd < NOFILE; fd++) {
    if (!p->ofile[fd]) {
      p->ofile[fd] = f;
      return fd;
    }
  }
  return -EMFILE;
}

static bool is_dot_or_dotdot(const char *name) {
  return strcmp(name, ".") == 0 || strcmp(name, "..") == 0;
}

i64 sys_dup(void) {
  struct file *f;
  int r = argfd(0, NULL, &f);
  if (r < 0) return r;
  int fd = fdalloc(f);
  if (fd < 0) return fd;
  filedup(f);
  return fd;
}

i64 sys_dup2(void) {
  struct file *f;
  int oldfd, r = argfd(0, &oldfd, &f);
  if (r < 0) return r;
  int newfd = (int)argraw(1);
  if (newfd < 0 || newfd >= NOFILE) return -EBADF;
  if (newfd == oldfd) return newfd;
  struct proc *p = myproc();
  if (p->ofile[newfd]) fileclose(p->ofile[newfd]);
  p->ofile[newfd] = filedup(f);
  return newfd;
}

i64 sys_read(void) {
  struct file *f;
  int r = argfd(0, NULL, &f);
  if (r < 0) return r;
  i64 n = (i64)argraw(2);
  if (n < 0) return -EINVAL;
  return fileread(f, argraw(1), (u64)n);
}

i64 sys_write(void) {
  struct file *f;
  int r = argfd(0, NULL, &f);
  if (r < 0) return r;
  i64 n = (i64)argraw(2);
  if (n < 0) return -EINVAL;
  return filewrite(f, argraw(1), (u64)n);
}

i64 sys_close(void) {
  struct file *f;
  int fd, r = argfd(0, &fd, &f);
  if (r < 0) return r;
  myproc()->ofile[fd] = NULL;
  fileclose(f);
  return 0;
}

i64 sys_fstat(void) {
  struct file *f;
  int r = argfd(0, NULL, &f);
  if (r < 0) return r;
  return filestat(f, argraw(1));
}

i64 sys_lseek(void) {
  struct file *f;
  int r = argfd(0, NULL, &f);
  if (r < 0) return r;
  if (f->type != FD_INODE) return -ESPIPE;
  i64 off = (i64)argraw(1);
  int whence = (int)argraw(2);
  acquiresleep(&f->poslock);
  i64 base;
  if (whence == SEEK_SET) {
    base = 0;
  } else if (whence == SEEK_CUR) {
    base = (i64)f->off;
  } else if (whence == SEEK_END) {
    ilock(f->ip);
    base = f->ip->d.i_size;
    iunlock(f->ip);
  } else {
    releasesleep(&f->poslock);
    return -EINVAL;
  }
  i64 pos = base + off;
  if (pos < 0) {
    releasesleep(&f->poslock);
    return -EINVAL;
  }
  f->off = (u64)pos;
  releasesleep(&f->poslock);
  return pos;
}

i64 sys_getdents(void) {
  struct file *f;
  int r = argfd(0, NULL, &f);
  if (r < 0) return r;
  if (f->type != FD_INODE) return -ENOTDIR;
  acquiresleep(&f->poslock);
  ilock(f->ip);
  i64 n = S_ISDIR(f->ip->d.i_mode) ? dir_getdents(f->ip, &f->off, argraw(1), argraw(2)) : -ENOTDIR;
  iunlock(f->ip);
  releasesleep(&f->poslock);
  return n;
}

i64 sys_ftruncate(void) {
  struct file *f;
  int r = argfd(0, NULL, &f);
  if (r < 0) return r;
  i64 len = (i64)argraw(1);
  if (len < 0) return -EINVAL;
  if (f->type != FD_INODE || !f->writable) return -EINVAL;
  ilock(f->ip);
  r = S_ISREG(f->ip->d.i_mode) ? itrunc(f->ip, (u64)len) : -EINVAL;
  iunlock(f->ip);
  return r;
}

i64 sys_pipe(void) {
  struct file *rf, *wf;
  int r = pipealloc(&rf, &wf);
  if (r < 0) return r;
  struct proc *p = myproc();
  int fds[2] = {-1, -1};
  if ((fds[0] = fdalloc(rf)) < 0 || (fds[1] = fdalloc(wf)) < 0) {
    if (fds[0] >= 0) p->ofile[fds[0]] = NULL;
    fileclose(rf);
    fileclose(wf);
    return -EMFILE;
  }
  if (copyout(p, argraw(0), fds, sizeof(fds)) < 0) {
    p->ofile[fds[0]] = p->ofile[fds[1]] = NULL;
    fileclose(rf);
    fileclose(wf);
    return -EFAULT;
  }
  return 0;
}

// Look up path's final element under ns_lock. Returns the new or existing
// inode locked. With excl, an existing name is an error.
static int create(const char *path, u16 mode, u32 rdev, bool excl, struct inode **out) {
  char name[NAME_MAX + 1];
  struct inode *dp;
  int r = nameiparent(path, name, &dp);
  if (r < 0) return r == -EBUSY ? -EEXIST : r;
  ilock(dp);
  struct inode *ip = dirlookup(dp, name, NULL);
  if (ip) {
    iunlockput(dp);
    if (excl) {
      iput(ip);
      return -EEXIST;
    }
    ilock(ip);
    *out = ip;
    return 0;
  }
  if (dp->d.i_links_count == 0) {   // directory has been removed
    iunlockput(dp);
    return -ENOENT;
  }
  r = ext2_create(dp, name, mode, rdev, &ip);
  iunlockput(dp);
  if (r < 0) return r;
  *out = ip;
  return 0;
}

i64 sys_open(void) {
  char path[MAXPATH];
  int r = argstr(0, path, sizeof(path));
  if (r < 0) return r;
  int flags = (int)argraw(1);
  u16 mode = (u16)(argraw(2) & 07777);
  int acc = flags & O_ACCMODE;
  if (acc == 3) return -EINVAL;

  struct inode *ip;
  if (flags & O_CREAT) {
    acquiresleep(&ns_lock);
    r = create(path, S_IFREG | mode, 0, (flags & O_EXCL) != 0, &ip);
    releasesleep(&ns_lock);
    if (r < 0) return r;
  } else {
    if ((r = namei(path, &ip)) < 0) return r;
    ilock(ip);
  }

  u16 m = ip->d.i_mode;
  if (S_ISDIR(m) && acc != O_RDONLY) r = -EISDIR;
  else if ((flags & O_DIRECTORY) && !S_ISDIR(m)) r = -ENOTDIR;
  else if (!S_ISDIR(m) && !S_ISREG(m) && !S_ISCHR(m)) r = -EACCES;
  if (r < 0) {
    iunlockput(ip);
    return r;
  }

  struct file *f = filealloc();
  if (!f) {
    iunlockput(ip);
    return -ENFILE;
  }
  int fd = fdalloc(f);
  if (fd < 0) {
    fileclose(f);
    iunlockput(ip);
    return fd;
  }
  if (S_ISCHR(m)) {
    struct stat st;
    stati(ip, &st);
    f->type = FD_DEVICE;
    f->rdev = st.st_rdev;
  } else {
    f->type = FD_INODE;
  }
  f->ip = ip;
  f->off = 0;
  f->readable = acc != O_WRONLY;
  f->writable = acc != O_RDONLY;
  f->append = (flags & O_APPEND) != 0;
  if ((flags & O_TRUNC) && S_ISREG(m) && f->writable) itrunc(ip, 0);
  iunlock(ip);
  return fd;
}

i64 sys_mkdir(void) {
  char path[MAXPATH];
  int r = argstr(0, path, sizeof(path));
  if (r < 0) return r;
  struct inode *ip;
  acquiresleep(&ns_lock);
  r = create(path, S_IFDIR | (u16)(argraw(1) & 07777), 0, true, &ip);
  releasesleep(&ns_lock);
  if (r < 0) return r;
  iunlockput(ip);
  return 0;
}

i64 sys_mknod(void) {
  char path[MAXPATH];
  int r = argstr(0, path, sizeof(path));
  if (r < 0) return r;
  u32 mode = (u32)argraw(1);
  if (!S_ISCHR(mode)) return -EINVAL;   // only character devices
  struct inode *ip;
  acquiresleep(&ns_lock);
  r = create(path, (u16)mode, (u32)argraw(2), true, &ip);
  releasesleep(&ns_lock);
  if (r < 0) return r;
  iunlockput(ip);
  return 0;
}

i64 sys_link(void) {
  char old[MAXPATH], new[MAXPATH], name[NAME_MAX + 1];
  int r;
  if ((r = argstr(0, old, sizeof(old))) < 0 || (r = argstr(1, new, sizeof(new))) < 0) return r;

  acquiresleep(&ns_lock);
  struct inode *ip, *dp;
  if ((r = namei(old, &ip)) < 0) goto out;
  ilock(ip);
  if (S_ISDIR(ip->d.i_mode)) {
    iunlockput(ip);
    r = -EPERM;
    goto out;
  }
  if (ip->d.i_links_count >= 65000) {
    iunlockput(ip);
    r = -EMLINK;
    goto out;
  }
  iunlock(ip);

  if ((r = nameiparent(new, name, &dp)) < 0) {
    iput(ip);
    if (r == -EBUSY) r = -EEXIST;
    goto out;
  }
  ilock(dp);
  struct inode *existing = dirlookup(dp, name, NULL);
  if (existing) {
    iput(existing);
    r = -EEXIST;
  } else if (dp->d.i_links_count == 0) {
    r = -ENOENT;
  } else {
    r = dirlink(dp, name, ip->inum, ip->d.i_mode);
  }
  iunlockput(dp);
  if (r == 0) {
    ilock(ip);
    ip->d.i_links_count++;
    ip->d.i_ctime = (u32)rtc_now();
    iupdate(ip);
    iunlock(ip);
  }
  iput(ip);
out:
  releasesleep(&ns_lock);
  return r;
}

i64 sys_unlink(void) {
  char path[MAXPATH], name[NAME_MAX + 1];
  int r = argstr(0, path, sizeof(path));
  if (r < 0) return r;

  acquiresleep(&ns_lock);
  struct inode *dp, *ip;
  if ((r = nameiparent(path, name, &dp)) < 0) {
    if (r == -EBUSY) r = -EISDIR;
    goto out;
  }
  if (is_dot_or_dotdot(name)) {
    iput(dp);
    r = -EISDIR;
    goto out;
  }
  ilock(dp);
  u32 off;
  ip = dirlookup(dp, name, &off);
  if (!ip) {
    iunlockput(dp);
    r = -ENOENT;
    goto out;
  }
  ilock(ip);
  if (S_ISDIR(ip->d.i_mode)) {
    r = -EISDIR;
  } else if ((r = dirunlink(dp, off)) == 0) {
    ip->d.i_links_count--;
    ip->d.i_ctime = (u32)rtc_now();
    iupdate(ip);
  }
  iunlockput(ip);
  iunlockput(dp);
out:
  releasesleep(&ns_lock);
  return r;
}

i64 sys_rmdir(void) {
  char path[MAXPATH], name[NAME_MAX + 1];
  int r = argstr(0, path, sizeof(path));
  if (r < 0) return r;

  acquiresleep(&ns_lock);
  struct inode *dp, *ip;
  if ((r = nameiparent(path, name, &dp)) < 0) goto out;
  if (is_dot_or_dotdot(name)) {
    iput(dp);
    r = strcmp(name, ".") == 0 ? -EINVAL : -ENOTEMPTY;
    goto out;
  }
  ilock(dp);
  u32 off;
  ip = dirlookup(dp, name, &off);
  if (!ip) {
    iunlockput(dp);
    r = -ENOENT;
    goto out;
  }
  ilock(ip);
  if (!S_ISDIR(ip->d.i_mode)) r = -ENOTDIR;
  else if (!dir_isempty(ip)) r = -ENOTEMPTY;
  else if ((r = dirunlink(dp, off)) == 0) {
    dp->d.i_links_count--;   // the child's ".." is gone
    iupdate(dp);
    ip->d.i_links_count = 0;
    iupdate(ip);
  }
  iunlockput(ip);
  iunlockput(dp);
out:
  releasesleep(&ns_lock);
  return r;
}

// Is dir an ancestor of (or the same as) dp? Walks ".." to the root with no
// locks held; ns_lock keeps the tree still.
static bool is_ancestor(u32 dir_inum, struct inode *dp) {
  struct inode *ip = idup(dp);
  bool found = false;
  for (;;) {
    if (ip->inum == dir_inum) {
      found = true;
      break;
    }
    if (ip->inum == EXT2_ROOT_INO) break;
    ilock(ip);
    struct inode *parent = dirlookup(ip, "..", NULL);
    iunlockput(ip);
    if (!parent) return false;
    ip = parent;
  }
  iput(ip);
  return found;
}

i64 sys_rename(void) {
  char old[MAXPATH], new[MAXPATH], oname[NAME_MAX + 1], nname[NAME_MAX + 1];
  int r;
  if ((r = argstr(0, old, sizeof(old))) < 0 || (r = argstr(1, new, sizeof(new))) < 0) return r;

  acquiresleep(&ns_lock);
  struct inode *odp = NULL, *ndp = NULL, *ip = NULL, *target = NULL;
  if ((r = nameiparent(old, oname, &odp)) < 0) goto out;
  if ((r = nameiparent(new, nname, &ndp)) < 0) goto out;
  if (is_dot_or_dotdot(oname) || is_dot_or_dotdot(nname)) {
    r = -EINVAL;
    goto out;
  }

  // Find the source and check for cycles before taking any locks.
  ilock(odp);
  u32 ooff;
  ip = dirlookup(odp, oname, &ooff);
  iunlock(odp);
  if (!ip) {
    r = -ENOENT;
    goto out;
  }
  ilock(ip);
  bool isdir = S_ISDIR(ip->d.i_mode);
  iunlock(ip);
  if (isdir && is_ancestor(ip->inum, ndp)) {
    r = -EINVAL;   // would move a directory into itself
    goto out;
  }

  ilock(ndp);
  u32 noff;
  target = dirlookup(ndp, nname, &noff);
  iunlock(ndp);
  if (target == ip) {
    r = 0;   // same file: nothing to do
    goto out;
  }
  if (target == odp || target == ndp) {
    r = isdir ? -ENOTEMPTY : -EISDIR;
    goto out;
  }
  if (ndp->d.i_links_count == 0) {
    r = -ENOENT;
    goto out;
  }

  ilock(odp);
  if (ndp != odp) ilock(ndp);
  ilock(ip);
  if (target) {
    ilock(target);
    if (isdir && !S_ISDIR(target->d.i_mode)) r = -ENOTDIR;
    else if (!isdir && S_ISDIR(target->d.i_mode)) r = -EISDIR;
    else if (isdir && !dir_isempty(target)) r = -ENOTEMPTY;
    if (r == 0) {
      // Point the existing entry at the source in place: no allocation, so
      // replacing a file cannot fail halfway.
      r = dir_set_entry(ndp, noff, ip->inum, ip->d.i_mode);
      if (r == 0) {
        if (S_ISDIR(target->d.i_mode)) {
          target->d.i_links_count = 0;
          ndp->d.i_links_count--;
          iupdate(ndp);
        } else {
          target->d.i_links_count--;
        }
        iupdate(target);
      }
    }
    iunlock(target);
  } else {
    r = dirlink(ndp, nname, ip->inum, ip->d.i_mode);
  }
  if (r == 0) r = dirunlink(odp, ooff);
  if (r == 0 && isdir && odp != ndp) {
    dir_set_dotdot(ip, ndp->inum);
    odp->d.i_links_count--;
    ndp->d.i_links_count++;
    iupdate(odp);
    iupdate(ndp);
  }
  if (r == 0) {
    ip->d.i_ctime = (u32)rtc_now();
    iupdate(ip);
  }
  iunlock(ip);
  if (ndp != odp) iunlock(ndp);
  iunlock(odp);

out:
  if (target) iput(target);
  if (ip) iput(ip);
  if (ndp) iput(ndp);
  if (odp) iput(odp);
  releasesleep(&ns_lock);
  return r;
}

i64 sys_chdir(void) {
  char path[MAXPATH];
  int r = argstr(0, path, sizeof(path));
  if (r < 0) return r;
  struct inode *ip;
  if ((r = namei(path, &ip)) < 0) return r;
  ilock(ip);
  if (!S_ISDIR(ip->d.i_mode)) {
    iunlockput(ip);
    return -ENOTDIR;
  }
  iunlock(ip);
  struct proc *p = myproc();
  iput(p->cwd);
  p->cwd = ip;
  return 0;
}

i64 sys_stat(void) {
  char path[MAXPATH];
  int r = argstr(0, path, sizeof(path));
  if (r < 0) return r;
  struct inode *ip;
  if ((r = namei(path, &ip)) < 0) return r;
  struct stat st;
  ilock(ip);
  stati(ip, &st);
  iunlockput(ip);
  return copyout(myproc(), argraw(1), &st, sizeof(st));
}

i64 sys_getcwd(void) {
  u64 dst = argraw(0);
  int size = (int)argraw(1);
  if (size <= 0) return -EINVAL;
  char *buf = kalloc();
  if (!buf) return -ENOMEM;
  int r = fs_getcwd(myproc()->cwd, buf, MIN(size, PGSIZE));
  if (r == 0) r = copyout(myproc(), dst, buf, strlen(buf) + 1);
  kfree(buf);
  return r;
}
