// usertests: the kernel's test suite, run inside kiln.
//
// Each test runs in its own child process, so a crash fails only that test.
// After every test the parent checks that free physical pages, free disk
// blocks and free inodes are back exactly where they started: any kernel
// leak (a page table, a COW page, an indirect block, an inode) fails it.
//
//   usertests            run everything
//   usertests name...    run only the named tests
#include "user.h"

#define TMP "/tmp/ut"
#define NOFILE_USER 32

static void fail(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));
static void fail(const char *fmt, ...) {
  int saved = errno;
  printf("    FAIL: ");
  va_list ap;
  va_start(ap, fmt);
  vdprintf(1, fmt, ap);
  va_end(ap);
  printf(" (errno %d: %s)\n", saved, strerror(saved));
  exit(1);
}

#define EXPECT(cond) \
  do { if (!(cond)) fail("%s:%d: %s", __func__, __LINE__, #cond); } while (0)
#define EXPECT_ERR(expr, err) \
  do { errno = 0; long __r = (long)(expr); \
       if (__r != -1 || errno != (err)) fail("%s:%d: %s returned %ld, want errno %d", \
                                             __func__, __LINE__, #expr, __r, (err)); } while (0)

static struct sysinfo info(void) {
  struct sysinfo si;
  if (sysinfo(&si) < 0) fail("sysinfo");
  return si;
}

// Run fn in a child and return its wait status.
static int in_child(void (*fn)(void)) {
  pid_t pid = fork();
  if (pid < 0) fail("fork");
  if (pid == 0) {
    fn();
    exit(0);
  }
  int st;
  if (waitpid(pid, &st, 0) != pid) fail("waitpid");
  return st;
}

static void write_all(int fd, const void *buf, size_t n) {
  const char *p = buf;
  while (n) {
    ssize_t w = write(fd, p, n);
    if (w <= 0) fail("write");
    p += w;
    n -= (size_t)w;
  }
}

static size_t read_all(int fd, void *buf, size_t n) {
  char *p = buf;
  size_t got = 0;
  while (got < n) {
    ssize_t r = read(fd, p + got, n - got);
    if (r < 0) fail("read");
    if (r == 0) break;
    got += (size_t)r;
  }
  return got;
}

static void rmtree(const char *path) {
  struct stat st;
  if (stat(path, &st) < 0) return;
  if (!S_ISDIR(st.st_mode)) {
    unlink(path);
    return;
  }
  for (;;) {
    DIR *d = opendir(path);
    if (!d) return;
    char names[16][64];
    int n = 0;
    struct dirent *e;
    while (n < 16 && (e = readdir(d))) {
      if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
      strlcpy(names[n++], e->d_name, sizeof(names[0]));
    }
    closedir(d);
    if (!n) break;
    for (int i = 0; i < n; i++) {
      char child[PATH_MAX];
      snprintf(child, sizeof(child), "%s/%s", path, names[i]);
      rmtree(child);
    }
  }
  rmdir(path);
}

static uint32_t pattern(uint64_t i) {
  return (uint32_t)(i * 2654435761u ^ (i >> 7));
}

// Poll until no live process has this pid (or, with group, this pgid).
static void wait_gone(pid_t id, bool group) {
  for (int tries = 0; tries < 500; tries++) {
    static struct procinfo pi[64];
    int n = procinfo(pi, 64);
    bool alive = false;
    for (int i = 0; i < n; i++)
      if ((group ? pi[i].pgid : pi[i].pid) == id && pi[i].state != PS_ZOMBIE) alive = true;
    if (!alive) return;
    msleep(10);
  }
  fail("process %d still alive", id);
}

// =========================================================== processes

static void t_fork_wait(void) {
  pid_t pids[20];
  for (int i = 0; i < 20; i++) {
    pids[i] = fork();
    EXPECT(pids[i] >= 0);
    if (pids[i] == 0) exit(i + 1);
  }
  // Reap in reverse with waitpid(pid) and check every status.
  for (int i = 19; i >= 0; i--) {
    int st;
    EXPECT(waitpid(pids[i], &st, 0) == pids[i]);
    EXPECT(WIFEXITED(st) && WEXITSTATUS(st) == i + 1);
  }
  EXPECT_ERR(wait(NULL), ECHILD);

  int p[2];
  EXPECT(pipe(p) == 0);
  pid_t slow = fork();
  if (slow == 0) {
    close(p[1]);   // or our own copy would keep the pipe open forever
    char c;
    read(p[0], &c, 1);
    exit(7);
  }
  int st;
  EXPECT(waitpid(slow, &st, WNOHANG) == 0);   // still running
  close(p[1]);
  EXPECT(waitpid(slow, &st, 0) == slow && WEXITSTATUS(st) == 7);
  close(p[0]);
}

static void t_fork_exhaust(void) {
  int p[2];
  EXPECT(pipe(p) == 0);
  int n = 0;
  for (;;) {
    pid_t pid = fork();
    if (pid < 0) {
      EXPECT(errno == EAGAIN);
      break;
    }
    if (pid == 0) {
      close(p[1]);
      char c;
      read(p[0], &c, 1);   // returns 0 once the parent closes the write end
      exit(0);
    }
    n++;
    EXPECT(n < 1000);
  }
  EXPECT(n > 10);
  close(p[1]);
  close(p[0]);
  for (int i = 0; i < n; i++) EXPECT(wait(NULL) > 0);
  EXPECT_ERR(wait(NULL), ECHILD);
  // The table drained: forking works again.
  pid_t pid = fork();
  if (pid == 0) exit(0);
  EXPECT(pid > 0 && wait(NULL) == pid);
}

static void t_reparent(void) {
  int p[2];
  EXPECT(pipe(p) == 0);
  pid_t mid = fork();
  if (mid == 0) {
    pid_t grand = fork();
    if (grand == 0) {
      msleep(50);   // let the middle process exit first
      int ids[2] = {getppid(), getpid()};
      write(p[1], ids, sizeof(ids));
      exit(0);
    }
    exit(0);
  }
  EXPECT(waitpid(mid, NULL, 0) == mid);
  close(p[1]);
  int ids[2] = {-1, -1};
  EXPECT(read_all(p[0], ids, sizeof(ids)) == sizeof(ids));
  EXPECT(ids[0] == 1);   // adopted by init
  close(p[0]);
  wait_gone(ids[1], false);
}

static void spin_forever(void) {
  for (volatile unsigned long i = 0;; i++) {}
}

static void t_kill(void) {
  pid_t pid = fork();
  if (pid == 0) spin_forever();
  msleep(20);
  EXPECT(kill(pid, SIGTERM) == 0);
  int st;
  EXPECT(waitpid(pid, &st, 0) == pid);
  EXPECT(WIFSIGNALED(st) && WTERMSIG(st) == SIGTERM);

  // A sleeping process wakes up to die.
  pid = fork();
  if (pid == 0) {
    msleep(100000);
    exit(0);
  }
  msleep(20);
  long t0 = uptime();
  EXPECT(kill(pid, SIGKILL) == 0);
  EXPECT(waitpid(pid, &st, 0) == pid && WTERMSIG(st) == SIGKILL);
  EXPECT(uptime() - t0 < 5000);

  EXPECT_ERR(kill(99999, SIGTERM), ESRCH);
  EXPECT_ERR(kill(getpid(), 0), EINVAL);

  // Signal a whole process group.
  pid_t a = fork();
  if (a == 0) {
    setpgid(0, 0);
    if (fork() == 0) spin_forever();
    spin_forever();
  }
  setpgid(a, a);
  msleep(30);
  EXPECT(kill(-a, SIGINT) == 0);
  EXPECT(waitpid(a, &st, 0) == a && WTERMSIG(st) == SIGINT);
  wait_gone(a, true);   // the grandchild was adopted by init
}

static void t_pgid(void) {
  EXPECT(getpgid(0) > 0);
  pid_t pid = fork();
  if (pid == 0) {
    msleep(50);
    exit(getpgid(0) == getpid() ? 0 : 1);
  }
  EXPECT(setpgid(pid, 0) == 0);
  EXPECT(getpgid(pid) == pid);
  int st;
  EXPECT(waitpid(pid, &st, 0) == pid && WEXITSTATUS(st) == 0);
  EXPECT_ERR(getpgid(99999), ESRCH);
}

static void t_sleep_time(void) {
  long t0 = uptime();
  EXPECT(msleep(100) == 0);
  long dt = uptime() - t0;
  EXPECT(dt >= 90 && dt < 3000);   // generous: CI hosts can be slow
  long now = time(NULL);
  EXPECT(now > 1700000000L);   // the RTC is set from the host
}

static void t_exec(void) {
  int p[2];
  EXPECT(pipe(p) == 0);
  pid_t pid = fork();
  if (pid == 0) {
    dup2(p[1], 1);
    close(p[0]);
    close(p[1]);
    char *argv[] = {"echo", "exec", "works", NULL};
    execv("/bin/echo", argv);
    exit(99);
  }
  close(p[1]);
  char buf[64] = {0};
  read_all(p[0], buf, sizeof(buf) - 1);
  close(p[0]);
  int st;
  EXPECT(waitpid(pid, &st, 0) == pid && WEXITSTATUS(st) == 0);
  EXPECT(strcmp(buf, "exec works\n") == 0);

  char *none[] = {"x", NULL};
  EXPECT_ERR(execv("/no/such/file", none), ENOENT);
  EXPECT_ERR(execv("/bin", none), EACCES);
  EXPECT_ERR(execv("/etc/motd", none), ENOEXEC);

  char *many[40];
  for (int i = 0; i < 39; i++) many[i] = "a";
  many[39] = NULL;
  EXPECT_ERR(execv("/bin/true", many), E2BIG);

  // 31 arguments is within MAXARG and runs.
  pid = fork();
  if (pid == 0) {
    many[31] = NULL;
    execv("/bin/true", many);
    exit(98);
  }
  EXPECT(waitpid(pid, &st, 0) == pid && WEXITSTATUS(st) == 0);
}

static void t_fork_exec_loop(void) {
  for (int i = 0; i < 60; i++) {
    pid_t pid = fork();
    EXPECT(pid >= 0);
    if (pid == 0) {
      char *argv[] = {"true", NULL};
      execv("/bin/true", argv);
      exit(1);
    }
    int st;
    EXPECT(waitpid(pid, &st, 0) == pid && WIFEXITED(st) && WEXITSTATUS(st) == 0);
  }
}

// =========================================================== memory

static char *grow(long n) {
  char *p = sbrk(n);
  if (p == (char *)-1) fail("sbrk(%ld)", n);
  return p;
}

static void t_sbrk_lazy(void) {
  long before = (long)info().free_pages;
  long n = 64L << 20;   // 64 MiB: half of RAM, but nothing is touched yet
  char *p = grow(n);
  EXPECT((long)info().free_pages > before - 8);
  // Touch one byte per MiB: exactly one page each gets allocated.
  for (long off = 0; off < n; off += 1 << 20) p[off] = 1;
  // 64 data pages plus one leaf page table per 2 MiB touched.
  long used = before - (long)info().free_pages;
  if (used < 64 + 30 || used >= 64 + 40) fail("sbrk used %ld pages", used);
  for (long off = 0; off < n; off += 1 << 20) EXPECT(p[off] == 1);
  EXPECT(p[4096] == 0);   // untouched lazy pages read as zero
  grow(-n);   // also frees the now-empty page tables
  EXPECT((long)info().free_pages >= before - 2);
}

static void t_sbrk_errors(void) {
  EXPECT(sbrk(1L << 45) == (void *)-1 && errno == ENOMEM);
  EXPECT(sbrk(-(1L << 40)) == (void *)-1 && errno == EINVAL);
  char *end = sbrk(0);
  // Past the break is not mapped.
  pid_t pid = fork();
  if (pid == 0) {
    volatile char *q = end + 8192;
    *q = 1;
    exit(0);
  }
  int st;
  EXPECT(waitpid(pid, &st, 0) == pid && WTERMSIG(st) == SIGSEGV);
}

static void t_cow(void) {
  long n = 256 * 4096;
  char *buf = grow(n);
  for (long i = 0; i < n; i += 4096) buf[i] = 'p';
  struct sysinfo s0 = info();

  int up[2], down[2];   // child -> parent, parent -> child
  EXPECT(pipe(up) == 0 && pipe(down) == 0);
  pid_t pid = fork();
  if (pid == 0) {
    for (long i = 0; i < n; i += 4096)
      if (buf[i] != 'p') exit(2);
    for (long i = 0; i < n; i += 4096) buf[i] = 'c';
    char ok = 'k';
    write(up[1], &ok, 1);
    char c;
    read(down[0], &c, 1);   // wait for the parent's writes
    for (long i = 0; i < n; i += 4096)
      if (buf[i] != 'c') exit(3);
    exit(0);
  }
  char c;
  EXPECT(read(up[0], &c, 1) == 1 && c == 'k');
  for (long i = 0; i < n; i += 4096) EXPECT(buf[i] == 'p');   // the child's writes are private
  for (long i = 0; i < n; i += 4096) buf[i] = 'q';
  write(down[1], &c, 1);
  int st;
  EXPECT(waitpid(pid, &st, 0) == pid && WIFEXITED(st) && WEXITSTATUS(st) == 0);
  for (long i = 0; i < n; i += 4096) EXPECT(buf[i] == 'q');
  struct sysinfo s1 = info();
  EXPECT(s1.cow_faults >= s0.cow_faults + 256);
  close(up[0]);
  close(up[1]);
  close(down[0]);
  close(down[1]);
  grow(-n);
}

static void t_cow_sharing(void) {
  // Four children share a 4 MiB heap: until they write, it costs almost
  // nothing; once they write, each pays for its own copy.
  long n = 4L << 20, pages = n / 4096;
  char *buf = grow(n);
  for (long i = 0; i < n; i += 4096) buf[i] = 1;
  long free0 = (long)info().free_pages;

  // One pipe per stage, so a fast child cannot eat a sibling's token.
  int go1[2], go2[2], done[2];
  EXPECT(pipe(go1) == 0 && pipe(go2) == 0 && pipe(done) == 0);
  pid_t kids[4];
  for (int k = 0; k < 4; k++) {
    kids[k] = fork();
    EXPECT(kids[k] >= 0);
    if (kids[k] == 0) {
      char c = 'r';
      write(done[1], &c, 1);
      read(go1[0], &c, 1);
      for (long i = 0; i < n; i += 4096) buf[i] = 2;
      write(done[1], &c, 1);
      read(go2[0], &c, 1);
      exit(0);
    }
  }
  char c;
  for (int k = 0; k < 4; k++) EXPECT(read(done[0], &c, 1) == 1);
  long shared = free0 - (long)info().free_pages;
  EXPECT(shared < 4 * 48);   // page tables, stacks and a few copied pages
  write_all(go1[1], "gggg", 4);
  for (int k = 0; k < 4; k++) EXPECT(read(done[0], &c, 1) == 1);
  long copied = free0 - (long)info().free_pages;
  EXPECT(copied >= 4 * pages);
  write_all(go2[1], "gggg", 4);
  for (int k = 0; k < 4; k++) EXPECT(waitpid(kids[k], NULL, 0) == kids[k]);
  int all[] = {go1[0], go1[1], go2[0], go2[1], done[0], done[1]};
  for (int i = 0; i < 6; i++) close(all[i]);
  grow(-n);
}

static int deep(int n) {
  char frame[2048];
  frame[0] = (char)n;
  frame[sizeof(frame) - 1] = (char)n;
  // Let the array's address escape so the compiler must keep all of it.
  asm volatile("" ::"r"(frame) : "memory");
  return n ? deep(n - 1) + frame[0] - frame[sizeof(frame) - 1] : 0;
}

static void stack_ok(void) {
  exit(deep(300) == 0 ? 0 : 1);   // ~640 KiB of stack, grown on demand
}

static void stack_overflow(void) {
  deep(1000);   // ~2 MiB: beyond the 1 MiB stack limit
  exit(0);
}

static void t_stack(void) {
  int st = in_child(stack_ok);
  EXPECT(WIFEXITED(st) && WEXITSTATUS(st) == 0);
  st = in_child(stack_overflow);
  if (!WIFSIGNALED(st) || WTERMSIG(st) != SIGSEGV) fail("stack overflow: status %#x", st);
}

static void null_read(void) { exit(*(volatile int *)0); }
static void null_write(void) { *(volatile int *)16 = 1; }
static void text_write(void) { *(volatile int *)(uintptr_t)&t_exec = 0; }
static void kernel_read(void) { exit((int)*(volatile long *)0xFFFFFFC080200000UL); }
static void illegal(void) { asm volatile(".word 0x00000000"); }
static void fpu(void) { asm volatile(".word 0x00007053"); }   // fadd.s: FPU is off
static void exec_heap(void) {
  uint32_t *code = malloc(16);
  code[0] = 0x00008067;   // ret
  ((void (*)(void))(uintptr_t)code)();   // heap pages are not executable
}

static void t_faults(void) {
  struct { void (*fn)(void); int sig; const char *name; } cases[] = {
    {null_read, SIGSEGV, "null read"},
    {null_write, SIGSEGV, "null write"},
    {text_write, SIGSEGV, "write to text"},
    {kernel_read, SIGSEGV, "read kernel memory"},
    {illegal, SIGILL, "illegal instruction"},
    {fpu, SIGILL, "floating point"},
    {exec_heap, SIGSEGV, "execute heap"},
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    int st = in_child(cases[i].fn);
    if (!WIFSIGNALED(st) || WTERMSIG(st) != cases[i].sig)
      fail("%s: status %#x, want signal %d", cases[i].name, st, cases[i].sig);
  }
}

static void oom(void) {
  for (;;) {
    char *p = sbrk(1 << 20);
    if (p == (char *)-1) exit(3);
    for (int i = 0; i < (1 << 20); i += 4096) p[i] = 1;
  }
}

static void t_oom(void) {
  // Use up all of RAM: the kernel must kill us, not panic, and get every
  // page back.
  int st = in_child(oom);
  EXPECT(WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL);
  // The system still works.
  pid_t pid = fork();
  if (pid == 0) exit(5);
  EXPECT(waitpid(pid, &st, 0) == pid && WEXITSTATUS(st) == 5);
}

// =========================================================== syscall arguments

static void t_bad_pointers(void) {
  int fd = open("/etc/motd", O_RDONLY);
  EXPECT(fd >= 0);
  EXPECT_ERR(read(fd, (void *)1, 100), EFAULT);
  EXPECT_ERR(read(fd, (void *)0xFFFFFFC080200000UL, 100), EFAULT);
  EXPECT_ERR(write(1, (void *)0x4000000000UL, 10), EFAULT);
  EXPECT_ERR(open((char *)0x10, O_RDONLY), EFAULT);
  EXPECT_ERR(pipe((int *)0x8), EFAULT);
  EXPECT_ERR(stat("/etc/motd", (struct stat *)0xFFFFFFC080200000UL), EFAULT);
  EXPECT_ERR(read(fd, NULL, (size_t)-1), EINVAL);
  EXPECT_ERR(read(77, NULL, 1), EBADF);
  EXPECT_ERR(close(-1), EBADF);
  close(fd);
  // Reading into the read-only text segment must fail, not corrupt it.
  fd = open("/etc/motd", O_RDONLY);
  EXPECT_ERR(read(fd, (void *)(uintptr_t)&t_exec, 16), EFAULT);
  close(fd);
  // A long path without a NUL in reach.
  char *big = malloc(8192);
  memset(big, 'a', 8192);
  EXPECT_ERR(open(big, O_RDONLY), ENAMETOOLONG);
  free(big);
}

// =========================================================== pipes

static void t_pipe_data(void) {
  int p[2];
  EXPECT(pipe(p) == 0);
  const long total = 200 * 1024;
  pid_t pid = fork();
  if (pid == 0) {
    close(p[0]);
    uint32_t buf[257];
    for (long i = 0; i < total / 4;) {
      int n = 0;
      for (; n < 257 && i < total / 4; n++, i++) buf[n] = pattern((uint64_t)i);
      write_all(p[1], buf, (size_t)n * 4);   // odd-sized writes wrap the ring
    }
    exit(0);
  }
  close(p[1]);
  uint32_t v;
  long i = 0;
  while (read_all(p[0], &v, 4) == 4) {
    if (v != pattern((uint64_t)i)) fail("pipe word %ld corrupted", i);
    i++;
  }
  EXPECT(i == total / 4);
  close(p[0]);
  EXPECT(waitpid(pid, NULL, 0) == pid);
}

static void t_pipe_eof_epipe(void) {
  int p[2];
  EXPECT(pipe(p) == 0);
  close(p[1]);
  char c;
  EXPECT(read(p[0], &c, 1) == 0);   // EOF: no writers
  close(p[0]);

  EXPECT(pipe(p) == 0);
  close(p[0]);
  pid_t pid = fork();
  if (pid == 0) {
    write(p[1], "x", 1);   // no readers: SIGPIPE
    exit(0);
  }
  int st;
  EXPECT(waitpid(pid, &st, 0) == pid && WIFSIGNALED(st) && WTERMSIG(st) == SIGPIPE);
  close(p[1]);
}

static void t_pipe_writers(void) {
  int p[2];
  EXPECT(pipe(p) == 0);
  for (int k = 0; k < 4; k++) {
    if (fork() == 0) {
      close(p[0]);
      for (int i = 0; i < 2000; i++) {
        uint32_t rec[2] = {(uint32_t)k, (uint32_t)i};
        write_all(p[1], rec, sizeof(rec));   // 8 bytes: never split
      }
      exit(0);
    }
  }
  close(p[1]);
  long seen[4] = {0}, sum[4] = {0};
  uint32_t rec[2];
  while (read_all(p[0], rec, sizeof(rec)) == sizeof(rec)) {
    EXPECT(rec[0] < 4);
    EXPECT(rec[1] == (uint32_t)seen[rec[0]]);   // in order per writer
    seen[rec[0]]++;
    sum[rec[0]] += rec[1];
  }
  for (int k = 0; k < 4; k++) EXPECT(seen[k] == 2000 && sum[k] == 1999L * 2000 / 2);
  for (int k = 0; k < 4; k++) EXPECT(wait(NULL) > 0);
  close(p[0]);
}

// =========================================================== files

static void t_file_basic(void) {
  mkdir(TMP, 0755);
  int fd = open(TMP "/a", O_RDWR | O_CREAT | O_EXCL, 0644);
  EXPECT(fd >= 0);
  write_all(fd, "hello, world", 12);
  EXPECT(lseek(fd, 0, SEEK_CUR) == 12);
  EXPECT(lseek(fd, 7, SEEK_SET) == 7);
  char buf[32] = {0};
  EXPECT(read(fd, buf, sizeof(buf)) == 5 && memcmp(buf, "world", 5) == 0);
  EXPECT(read(fd, buf, sizeof(buf)) == 0);   // EOF
  EXPECT(lseek(fd, -5, SEEK_END) == 7);
  EXPECT_ERR(lseek(fd, -100, SEEK_CUR), EINVAL);
  close(fd);

  EXPECT_ERR(open(TMP "/a", O_CREAT | O_EXCL | O_WRONLY, 0644), EEXIST);
  EXPECT_ERR(open(TMP "/missing", O_RDONLY), ENOENT);
  EXPECT_ERR(open(TMP "/a/x", O_RDONLY), ENOTDIR);
  EXPECT_ERR(open(TMP, O_WRONLY), EISDIR);
  EXPECT_ERR(open(TMP "/a", O_RDONLY | O_DIRECTORY), ENOTDIR);

  fd = open(TMP "/a", O_WRONLY | O_APPEND);
  write_all(fd, "!", 1);
  lseek(fd, 0, SEEK_SET);
  write_all(fd, "?", 1);   // O_APPEND ignores the seek
  close(fd);
  struct stat st;
  EXPECT(stat(TMP "/a", &st) == 0 && st.st_size == 14 && S_ISREG(st.st_mode));
  EXPECT(st.st_nlink == 1 && (st.st_mode & 0777) == 0644);

  fd = open(TMP "/a", O_RDONLY);
  EXPECT(read(fd, buf, sizeof(buf)) == 14 && memcmp(buf, "hello, world!?", 14) == 0);
  EXPECT_ERR(write(fd, "x", 1), EBADF);   // opened read-only
  close(fd);

  fd = open(TMP "/a", O_WRONLY | O_TRUNC);
  EXPECT(fstat(fd, &st) == 0 && st.st_size == 0 && st.st_blocks == 0);
  close(fd);
  EXPECT(unlink(TMP "/a") == 0);
  EXPECT_ERR(unlink(TMP "/a"), ENOENT);
  EXPECT(rmdir(TMP) == 0);
}

// Write a file large enough to need double-indirect blocks (1 KiB blocks:
// 12 direct + 256 single-indirect = 268 KiB), read it back, delete it.
static void t_file_big(void) {
  mkdir(TMP, 0755);
  const long size = 1536 * 1024 + 123;
  struct sysinfo s0 = info();
  int fd = open(TMP "/big", O_WRONLY | O_CREAT | O_TRUNC, 0644);
  EXPECT(fd >= 0);
  static uint32_t buf[1024];
  for (long off = 0; off < size;) {
    long n = MIN((long)sizeof(buf), size - off);
    for (long i = 0; i < n / 4 + 1 && i < 1024; i++) buf[i] = pattern((uint64_t)(off / 4 + i));
    write_all(fd, buf, (size_t)n);
    off += n;
  }
  close(fd);
  struct stat st;
  EXPECT(stat(TMP "/big", &st) == 0 && (long)st.st_size == size);
  // Data blocks plus the indirect blocks that map them (512-byte units).
  long data = (size + 1023) / 1024;
  long ind = 1 + 1 + (data - 12 - 256 + 255) / 256;
  EXPECT((long)st.st_blocks == 2 * (data + ind));
  EXPECT((long)(s0.fs_free_blocks - info().fs_free_blocks) == data + ind);

  fd = open(TMP "/big", O_RDONLY);
  for (long off = 0; off < size;) {
    long n = MIN((long)sizeof(buf), size - off);
    EXPECT((long)read_all(fd, buf, (size_t)n) == n);
    for (long i = 0; i < n / 4; i++)
      if (buf[i] != pattern((uint64_t)(off / 4 + i))) fail("big file word %ld", off / 4 + i);
    off += n;
  }
  close(fd);
  EXPECT(unlink(TMP "/big") == 0);
  rmdir(TMP);
}

static void t_file_sparse(void) {
  mkdir(TMP, 0755);
  int fd = open(TMP "/sparse", O_RDWR | O_CREAT | O_TRUNC, 0644);
  EXPECT(lseek(fd, 5L << 20, SEEK_SET) == 5L << 20);
  write_all(fd, "Z", 1);
  struct stat st;
  EXPECT(fstat(fd, &st) == 0 && st.st_size == (5L << 20) + 1);
  EXPECT(st.st_blocks <= 2 * 4);   // one data block plus its indirect chain
  char buf[4096];
  lseek(fd, 3L << 20, SEEK_SET);
  EXPECT(read(fd, buf, sizeof(buf)) == sizeof(buf));
  for (size_t i = 0; i < sizeof(buf); i++) EXPECT(buf[i] == 0);
  lseek(fd, 5L << 20, SEEK_SET);
  EXPECT(read(fd, buf, 10) == 1 && buf[0] == 'Z');
  // Past 64 MiB + 268 KiB (with 1 KiB blocks) the mapping needs the
  // triple-indirect tree: three indirect blocks plus the data block.
  EXPECT(lseek(fd, 100L << 20, SEEK_SET) == 100L << 20);
  write_all(fd, "T", 1);
  EXPECT(fstat(fd, &st) == 0 && st.st_size == (100L << 20) + 1);
  EXPECT(st.st_blocks <= 2 * (4 + 4));
  lseek(fd, 100L << 20, SEEK_SET);
  EXPECT(read(fd, buf, 10) == 1 && buf[0] == 'T');
  close(fd);
  unlink(TMP "/sparse");
  rmdir(TMP);
}

static void t_ftruncate(void) {
  mkdir(TMP, 0755);
  int fd = open(TMP "/t", O_RDWR | O_CREAT | O_TRUNC, 0644);
  static char buf[3000];
  memset(buf, 'x', sizeof(buf));
  write_all(fd, buf, sizeof(buf));
  EXPECT(ftruncate(fd, 1500) == 0);
  struct stat st;
  EXPECT(fstat(fd, &st) == 0 && st.st_size == 1500 && st.st_blocks == 4);
  EXPECT(ftruncate(fd, 2500) == 0);   // grow: the gap must read as zeros
  lseek(fd, 0, SEEK_SET);
  EXPECT(read_all(fd, buf, sizeof(buf)) == 2500);
  for (int i = 0; i < 1500; i++) EXPECT(buf[i] == 'x');
  for (int i = 1500; i < 2500; i++) EXPECT(buf[i] == 0);
  EXPECT(ftruncate(fd, 0) == 0);
  EXPECT(fstat(fd, &st) == 0 && st.st_size == 0 && st.st_blocks == 0);
  EXPECT_ERR(ftruncate(fd, -1), EINVAL);
  close(fd);
  unlink(TMP "/t");
  rmdir(TMP);
}

static void t_unlink_open(void) {
  mkdir(TMP, 0755);
  struct sysinfo s0 = info();
  int fd = open(TMP "/u", O_RDWR | O_CREAT | O_TRUNC, 0644);
  static char buf[20000];
  memset(buf, 'u', sizeof(buf));
  write_all(fd, buf, sizeof(buf));
  EXPECT(unlink(TMP "/u") == 0);
  struct stat st;
  EXPECT_ERR(stat(TMP "/u", &st), ENOENT);
  EXPECT(fstat(fd, &st) == 0 && st.st_nlink == 0);
  lseek(fd, 0, SEEK_SET);
  memset(buf, 0, sizeof(buf));
  EXPECT(read_all(fd, buf, sizeof(buf)) == sizeof(buf) && buf[19999] == 'u');
  EXPECT(info().fs_free_inodes == s0.fs_free_inodes - 1);   // still allocated
  close(fd);
  EXPECT(info().fs_free_inodes == s0.fs_free_inodes);       // freed on last close
  rmdir(TMP);
}

static void t_links(void) {
  mkdir(TMP, 0755);
  int fd = open(TMP "/orig", O_WRONLY | O_CREAT, 0644);
  write_all(fd, "linked", 6);
  close(fd);
  EXPECT(link(TMP "/orig", TMP "/alias") == 0);
  struct stat a, b;
  EXPECT(stat(TMP "/orig", &a) == 0 && stat(TMP "/alias", &b) == 0);
  EXPECT(a.st_ino == b.st_ino && a.st_nlink == 2);
  EXPECT_ERR(link(TMP "/orig", TMP "/alias"), EEXIST);
  EXPECT_ERR(link(TMP, TMP "/dirlink"), EPERM);
  EXPECT_ERR(link(TMP "/nope", TMP "/x"), ENOENT);
  EXPECT(unlink(TMP "/orig") == 0);
  char buf[8] = {0};
  fd = open(TMP "/alias", O_RDONLY);
  EXPECT(read(fd, buf, sizeof(buf)) == 6 && memcmp(buf, "linked", 6) == 0);
  EXPECT(fstat(fd, &b) == 0 && b.st_nlink == 1);
  close(fd);
  unlink(TMP "/alias");
  rmdir(TMP);
}

static void t_dirs(void) {
  EXPECT(mkdir(TMP, 0755) == 0);
  EXPECT_ERR(mkdir(TMP, 0755), EEXIST);
  EXPECT(mkdir(TMP "/a", 0755) == 0);
  EXPECT(mkdir(TMP "/a/b", 0755) == 0);
  struct stat st;
  EXPECT(stat(TMP "/a", &st) == 0 && st.st_nlink == 3);   // ".", its entry, b's ".."
  EXPECT_ERR(rmdir(TMP "/a"), ENOTEMPTY);
  EXPECT_ERR(unlink(TMP "/a"), EISDIR);
  int fd = open(TMP "/a/f", O_WRONLY | O_CREAT, 0644);
  close(fd);
  EXPECT_ERR(rmdir(TMP "/a/f"), ENOTDIR);
  EXPECT_ERR(mkdir(TMP "/a/f/x", 0755), ENOTDIR);
  EXPECT_ERR(mkdir(TMP "/nope/x", 0755), ENOENT);
  EXPECT_ERR(rmdir(TMP "/a/b/."), EINVAL);
  EXPECT_ERR(rmdir(TMP "/a/b/.."), ENOTEMPTY);
  EXPECT_ERR(mkdir("/", 0755), EEXIST);

  char cwd[PATH_MAX], old[PATH_MAX];
  EXPECT(getcwd(old, sizeof(old)));
  EXPECT(chdir(TMP "/a/b") == 0);
  EXPECT(getcwd(cwd, sizeof(cwd)) && strcmp(cwd, TMP "/a/b") == 0);
  EXPECT(chdir("../..//a/./b/../../a") == 0);
  EXPECT(getcwd(cwd, sizeof(cwd)) && strcmp(cwd, TMP "/a") == 0);
  EXPECT(stat("f", &st) == 0 && S_ISREG(st.st_mode));
  EXPECT_ERR(chdir("f"), ENOTDIR);
  EXPECT_ERR(getcwd(cwd, 3) ? 0 : -1, ERANGE);
  EXPECT(chdir("/") == 0);
  EXPECT(getcwd(cwd, sizeof(cwd)) && strcmp(cwd, "/") == 0);

  // Removing the current directory leaves a working but empty cwd.
  EXPECT(chdir(TMP "/a/b") == 0);
  EXPECT(rmdir(TMP "/a/b") == 0);
  EXPECT_ERR(open("new", O_CREAT | O_WRONLY, 0644), ENOENT);
  EXPECT(chdir(old) == 0);

  EXPECT(unlink(TMP "/a/f") == 0);
  EXPECT(rmdir(TMP "/a") == 0);
  EXPECT(stat(TMP, &st) == 0 && st.st_nlink == 2);
  EXPECT(rmdir(TMP) == 0);
}

static int count_entries(const char *path) {
  DIR *d = opendir(path);
  if (!d) fail("opendir %s", path);
  int n = 0;
  while (readdir(d)) n++;
  closedir(d);
  return n;
}

static void t_dir_many(void) {
  // 400 entries need many directory blocks; delete every other one,
  // check the holes are reused, and that getdents sees each name once.
  EXPECT(mkdir(TMP, 0755) == 0);
  char path[64];
  for (int i = 0; i < 400; i++) {
    snprintf(path, sizeof(path), TMP "/file-with-a-long-name-%03d", i);
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
    EXPECT(fd >= 0);
    close(fd);
  }
  EXPECT(count_entries(TMP) == 402);
  struct stat st;
  EXPECT(stat(TMP, &st) == 0);
  long size = (long)st.st_size;
  for (int i = 0; i < 400; i += 2) {
    snprintf(path, sizeof(path), TMP "/file-with-a-long-name-%03d", i);
    EXPECT(unlink(path) == 0);
  }
  EXPECT(count_entries(TMP) == 202);
  for (int i = 0; i < 400; i += 2) {
    snprintf(path, sizeof(path), TMP "/x%03d", i);
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
    EXPECT(fd >= 0);
    close(fd);
  }
  EXPECT(stat(TMP, &st) == 0 && (long)st.st_size == size);   // no growth: slots reused
  bool seen[400] = {false};
  DIR *d = opendir(TMP);
  struct dirent *e;
  while ((e = readdir(d))) {
    int k;
    if (e->d_name[0] == 'x') k = atoi(e->d_name + 1);
    else if (e->d_name[0] == 'f') k = atoi(e->d_name + 22);
    else continue;
    EXPECT(k >= 0 && k < 400 && !seen[k]);
    EXPECT(e->d_type == DT_REG);
    seen[k] = true;
  }
  closedir(d);
  for (int i = 0; i < 400; i++) EXPECT(seen[i]);

  // A buffer too small for even one record.
  int fd = open(TMP, O_RDONLY);
  char tiny[8];
  EXPECT_ERR(getdents(fd, tiny, sizeof(tiny)), EINVAL);
  close(fd);
  rmtree(TMP);
}

static void t_rename(void) {
  EXPECT(mkdir(TMP, 0755) == 0);
  EXPECT(mkdir(TMP "/d1", 0755) == 0);
  EXPECT(mkdir(TMP "/d2", 0755) == 0);
  int fd = open(TMP "/d1/f", O_WRONLY | O_CREAT, 0644);
  write_all(fd, "one", 3);
  close(fd);
  EXPECT(rename(TMP "/d1/f", TMP "/d1/g") == 0);
  struct stat st;
  EXPECT_ERR(stat(TMP "/d1/f", &st), ENOENT);
  EXPECT(rename(TMP "/d1/g", TMP "/d2/g") == 0);   // across directories
  EXPECT(stat(TMP "/d2/g", &st) == 0 && st.st_size == 3);

  // Over an existing file: the old target goes away.
  fd = open(TMP "/d2/h", O_WRONLY | O_CREAT, 0644);
  write_all(fd, "replaced", 8);
  close(fd);
  struct sysinfo s0 = info();
  EXPECT(rename(TMP "/d2/g", TMP "/d2/h") == 0);
  EXPECT(stat(TMP "/d2/h", &st) == 0 && st.st_size == 3);
  EXPECT(info().fs_free_inodes == s0.fs_free_inodes + 1);

  // Move a directory: ".." and both parents' link counts follow.
  EXPECT(mkdir(TMP "/d1/sub", 0755) == 0);
  EXPECT(rename(TMP "/d1/sub", TMP "/d2/sub") == 0);
  EXPECT(stat(TMP "/d1", &st) == 0 && st.st_nlink == 2);
  EXPECT(stat(TMP "/d2", &st) == 0 && st.st_nlink == 3);
  char cwd[PATH_MAX], old[PATH_MAX];
  getcwd(old, sizeof(old));
  EXPECT(chdir(TMP "/d2/sub") == 0 && chdir("..") == 0);
  EXPECT(getcwd(cwd, sizeof(cwd)) && strcmp(cwd, TMP "/d2") == 0);
  chdir(old);

  EXPECT_ERR(rename(TMP "/d2", TMP "/d2/sub/inside"), EINVAL);
  EXPECT_ERR(rename(TMP "/d1", TMP "/d2/h"), ENOTDIR);
  EXPECT_ERR(rename(TMP "/d2/h", TMP "/d1"), EISDIR);
  EXPECT_ERR(rename(TMP "/d1", TMP "/d2"), ENOTEMPTY);
  EXPECT(rename(TMP "/d2/h", TMP "/d2/h") == 0);
  EXPECT(rename(TMP "/d1", TMP "/d2/sub") == 0);   // onto an empty directory
  EXPECT(stat(TMP "/d2", &st) == 0 && st.st_nlink == 3);
  rmtree(TMP);
}

static void t_dup(void) {
  mkdir(TMP, 0755);
  int fd = open(TMP "/dup", O_RDWR | O_CREAT | O_TRUNC, 0644);
  int fd2 = dup(fd);
  EXPECT(fd2 > fd);
  write_all(fd, "ab", 2);
  write_all(fd2, "cd", 2);   // shared offset
  EXPECT(lseek(fd, 0, SEEK_CUR) == 4);
  EXPECT(dup2(fd, 20) == 20);
  EXPECT(lseek(20, 0, SEEK_SET) == 0);
  char buf[8] = {0};
  EXPECT(read(fd2, buf, 8) == 4 && memcmp(buf, "abcd", 4) == 0);
  EXPECT(dup2(fd, fd) == fd);
  EXPECT_ERR(dup2(fd, 999), EBADF);
  close(fd);
  close(fd2);
  close(20);

  int fds[NOFILE_USER];
  int n = 0;
  for (;;) {
    int f = open("/etc/motd", O_RDONLY);
    if (f < 0) {
      EXPECT(errno == EMFILE);
      break;
    }
    fds[n++] = f;
    EXPECT(n < NOFILE_USER);
  }
  for (int i = 0; i < n; i++) close(fds[i]);
  unlink(TMP "/dup");
  rmdir(TMP);
}

static void t_devices(void) {
  int fd = open("/dev/null", O_RDWR);
  EXPECT(fd >= 0);
  EXPECT(write(fd, "discard", 7) == 7);
  char buf[64];
  EXPECT(read(fd, buf, sizeof(buf)) == 0);
  close(fd);
  fd = open("/dev/zero", O_RDONLY);
  memset(buf, 1, sizeof(buf));
  EXPECT(read(fd, buf, sizeof(buf)) == sizeof(buf));
  for (size_t i = 0; i < sizeof(buf); i++) EXPECT(buf[i] == 0);
  close(fd);
  struct stat st;
  EXPECT(stat("/dev/console", &st) == 0 && S_ISCHR(st.st_mode));
  EXPECT(DEV_MAJOR(st.st_rdev) == MAJOR_CONSOLE);
}

// =========================================================== concurrency

static void t_concurrent_files(void) {
  EXPECT(mkdir(TMP, 0755) == 0);
  for (int k = 0; k < 4; k++) {
    if (fork() == 0) {
      char path[64];
      static char buf[3000];
      for (int it = 0; it < 30; it++) {
        snprintf(path, sizeof(path), TMP "/w%d-%d", k, it);
        int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) exit(1);
        memset(buf, 'a' + k, sizeof(buf));
        write_all(fd, buf, sizeof(buf));
        lseek(fd, 0, SEEK_SET);
        memset(buf, 0, sizeof(buf));
        if (read_all(fd, buf, sizeof(buf)) != sizeof(buf) || buf[2999] != 'a' + k) exit(2);
        close(fd);
        if (it % 3 == 0 && unlink(path) < 0) exit(3);
      }
      exit(0);
    }
  }
  // Meanwhile, four appenders share one file.
  for (int k = 0; k < 4; k++) {
    if (fork() == 0) {
      int fd = open(TMP "/shared", O_WRONLY | O_CREAT | O_APPEND, 0644);
      char line[16];
      for (int i = 0; i < 100; i++) {
        snprintf(line, sizeof(line), "%d:%03d\n", k, i);
        write_all(fd, line, 6);
      }
      close(fd);
      exit(0);
    }
  }
  for (int k = 0; k < 8; k++) {
    int st;
    EXPECT(wait(&st) > 0);
    EXPECT(WIFEXITED(st) && WEXITSTATUS(st) == 0);
  }
  struct stat st;
  EXPECT(stat(TMP "/shared", &st) == 0 && st.st_size == 4 * 100 * 6);
  EXPECT(count_entries(TMP) == 2 + 4 * 20 + 1);
  rmtree(TMP);
}

static void t_smp(void) {
  struct sysinfo si = info();
  if (si.ncpu < 2) return;
  pid_t kids[4];
  for (int k = 0; k < 4; k++) {
    kids[k] = fork();
    if (kids[k] == 0) spin_forever();
  }
  unsigned seen = 0;
  long t0 = uptime();
  while (uptime() - t0 < 1000 && __builtin_popcount(seen) < 2) {
    static struct procinfo pi[64];
    int n = procinfo(pi, 64);
    for (int i = 0; i < n; i++)
      if (pi[i].state == PS_RUNNING && pi[i].cpu >= 0) seen |= 1u << pi[i].cpu;
  }
  for (int k = 0; k < 4; k++) kill(kids[k], SIGKILL);
  for (int k = 0; k < 4; k++) waitpid(kids[k], NULL, 0);
  EXPECT(__builtin_popcount(seen) >= 2);
}

// =========================================================== runner

struct test {
  const char *name;
  void (*fn)(void);
};

static const struct test tests[] = {
  {"fork_wait", t_fork_wait},
  {"fork_exhaust", t_fork_exhaust},
  {"reparent", t_reparent},
  {"kill", t_kill},
  {"pgid", t_pgid},
  {"sleep_time", t_sleep_time},
  {"exec", t_exec},
  {"fork_exec_loop", t_fork_exec_loop},
  {"sbrk_lazy", t_sbrk_lazy},
  {"sbrk_errors", t_sbrk_errors},
  {"cow", t_cow},
  {"cow_sharing", t_cow_sharing},
  {"stack", t_stack},
  {"faults", t_faults},
  {"bad_pointers", t_bad_pointers},
  {"pipe_data", t_pipe_data},
  {"pipe_eof_epipe", t_pipe_eof_epipe},
  {"pipe_writers", t_pipe_writers},
  {"file_basic", t_file_basic},
  {"file_big", t_file_big},
  {"file_sparse", t_file_sparse},
  {"ftruncate", t_ftruncate},
  {"unlink_open", t_unlink_open},
  {"links", t_links},
  {"dirs", t_dirs},
  {"dir_many", t_dir_many},
  {"rename", t_rename},
  {"dup", t_dup},
  {"devices", t_devices},
  {"concurrent_files", t_concurrent_files},
  {"smp", t_smp},
  {"oom", t_oom},
};

int main(int argc, char *argv[]) {
  int failed = 0, ran = 0;
  long t_start = uptime();
  // Grow the heap once up front so the runner's own mallocs (opendir in
  // rmtree) never show up as leaks.
  free(malloc(16384));
  rmtree(TMP);
  for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
    const struct test *t = &tests[i];
    if (argc > 1) {
      bool wanted = false;
      for (int a = 1; a < argc; a++) wanted |= strcmp(argv[a], t->name) == 0;
      if (!wanted) continue;
    }
    ran++;
    printf("test %-18s ", t->name);
    fflush(1);
    struct sysinfo before = info();
    long t0 = uptime();
    int st = in_child(t->fn);
    rmtree(TMP);
    struct sysinfo after = info();
    bool ok = WIFEXITED(st) && WEXITSTATUS(st) == 0;
    if (!ok && WIFSIGNALED(st)) printf("    FAIL: killed by signal %d\n", WTERMSIG(st));
    if (ok && after.free_pages != before.free_pages) {
      printf("    FAIL: leaked %ld pages\n", (long)before.free_pages - (long)after.free_pages);
      ok = false;
    }
    if (ok && (after.fs_free_blocks != before.fs_free_blocks ||
               after.fs_free_inodes != before.fs_free_inodes)) {
      printf("    FAIL: leaked %ld blocks, %ld inodes\n",
             (long)before.fs_free_blocks - (long)after.fs_free_blocks,
             (long)before.fs_free_inodes - (long)after.fs_free_inodes);
      ok = false;
    }
    if (ok) printf("ok  %4ld ms\n", uptime() - t0);
    else failed++;
  }
  if (failed) printf("%d OF %d TESTS FAILED\n", failed, ran);
  else printf("ALL %d TESTS PASSED in %ld ms\n", ran, uptime() - t_start);
  return failed ? 1 : 0;
}
