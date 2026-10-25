// Small libc pieces built on the raw system calls.
#include "user.h"

int errno;

long __sbrk(long n);
int __open(const char *path, int flags, int mode);
pid_t __fork(void);
void __flush_stdout(void);

pid_t fork(void) {
  __flush_stdout();
  return __fork();
}

void exit(int status) {
  __flush_stdout();
  _exit(status);
}

void *sbrk(long n) {
  return (void *)__sbrk(n);
}

int open(const char *path, int flags, ...) {
  int mode = 0;
  if (flags & O_CREAT) {
    va_list ap;
    va_start(ap, flags);
    mode = va_arg(ap, int);
    va_end(ap);
  }
  return __open(path, flags, mode);
}

int creat(const char *path, int mode) {
  return open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
}

pid_t wait(int *status) {
  return waitpid(-1, status, 0);
}

int execv(const char *path, char *const argv[]) {
  return execve(path, argv);
}

// Search /bin for names without a slash.
int execvp(const char *file, char *const argv[]) {
  if (strchr(file, '/')) return execve(file, argv);
  char path[PATH_MAX];
  snprintf(path, sizeof(path), "/bin/%s", file);
  return execve(path, argv);
}

unsigned sleep(unsigned seconds) {
  return msleep((unsigned long)seconds * 1000) < 0 ? seconds : 0;
}

long time(long *t) {
  long now = __sys_time();
  if (t) *t = now;
  return now;
}

char *getcwd(char *buf, size_t size) {
  long r = __sys_getcwd(buf, size);
  if (r < 0) {
    errno = (int)-r;
    return NULL;
  }
  return buf;
}

int isatty(int fd) {
  struct stat st;
  if (fstat(fd, &st) < 0) return 0;
  return S_ISCHR(st.st_mode) && DEV_MAJOR(st.st_rdev) == MAJOR_CONSOLE;
}

int isdigit(int c) { return c >= '0' && c <= '9'; }
int isspace(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f'; }
int isalpha(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
int isalnum(int c) { return isalpha(c) || isdigit(c); }
int isprint(int c) { return c >= 0x20 && c < 0x7f; }
int tolower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
int toupper(int c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }

unsigned long strtoul(const char *s, char **end, int base) {
  while (isspace(*s)) s++;
  if (*s == '+') s++;
  if ((base == 0 || base == 16) && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
    s += 2;
    base = 16;
  } else if (base == 0 && s[0] == '0' && s[1]) {
    base = 8;
  } else if (base == 0) {
    base = 10;
  }
  unsigned long v = 0;
  for (;; s++) {
    int d;
    if (isdigit(*s)) d = *s - '0';
    else if (*s >= 'a' && *s <= 'z') d = *s - 'a' + 10;
    else if (*s >= 'A' && *s <= 'Z') d = *s - 'A' + 10;
    else break;
    if (d >= base) break;
    v = v * (unsigned long)base + (unsigned long)d;
  }
  if (end) *end = (char *)s;
  return v;
}

long strtol(const char *s, char **end, int base) {
  while (isspace(*s)) s++;
  bool neg = *s == '-';
  if (neg) s++;
  unsigned long v = strtoul(s, end, base);
  return neg ? -(long)v : (long)v;
}

int atoi(const char *s) {
  return (int)strtol(s, NULL, 10);
}

char *strdup(const char *s) {
  size_t n = strlen(s) + 1;
  char *p = malloc(n);
  if (p) memcpy(p, s, n);
  return p;
}

const char *basename_of(const char *path) {
  const char *slash = strrchr(path, '/');
  return slash && slash[1] ? slash + 1 : path;
}

static void swap_bytes(char *a, char *b, size_t n) {
  while (n--) {
    char t = *a;
    *a++ = *b;
    *b++ = t;
  }
}

// Introsort-free quicksort with insertion sort for small ranges.
void qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *)) {
  char *a = base;
  while (n > 8) {
    char *pivot = a + (n / 2) * size;
    swap_bytes(pivot, a + (n - 1) * size, size);
    pivot = a + (n - 1) * size;
    size_t store = 0;
    for (size_t i = 0; i + 1 < n; i++) {
      if (cmp(a + i * size, pivot) < 0) {
        swap_bytes(a + i * size, a + store * size, size);
        store++;
      }
    }
    swap_bytes(a + store * size, pivot, size);
    // Recurse into the smaller half, loop on the larger.
    if (store < n - store - 1) {
      qsort(a, store, size, cmp);
      a += (store + 1) * size;
      n -= store + 1;
    } else {
      qsort(a + (store + 1) * size, n - store - 1, size, cmp);
      n = store;
    }
  }
  for (size_t i = 1; i < n; i++)
    for (size_t j = i; j > 0 && cmp(a + (j - 1) * size, a + j * size) > 0; j--)
      swap_bytes(a + (j - 1) * size, a + j * size, size);
}

static const char *const errnames[] = {
  [0] = "Success",
  [EPERM] = "Operation not permitted",
  [ENOENT] = "No such file or directory",
  [ESRCH] = "No such process",
  [EINTR] = "Interrupted system call",
  [EIO] = "I/O error",
  [E2BIG] = "Argument list too long",
  [ENOEXEC] = "Exec format error",
  [EBADF] = "Bad file descriptor",
  [ECHILD] = "No child processes",
  [EAGAIN] = "Resource temporarily unavailable",
  [ENOMEM] = "Out of memory",
  [EACCES] = "Permission denied",
  [EFAULT] = "Bad address",
  [EBUSY] = "Device or resource busy",
  [EEXIST] = "File exists",
  [EXDEV] = "Cross-device link",
  [ENODEV] = "No such device",
  [ENOTDIR] = "Not a directory",
  [EISDIR] = "Is a directory",
  [EINVAL] = "Invalid argument",
  [ENFILE] = "Too many open files in system",
  [EMFILE] = "Too many open files",
  [ENOTTY] = "Not a terminal",
  [EFBIG] = "File too large",
  [ENOSPC] = "No space left on device",
  [ESPIPE] = "Illegal seek",
  [EROFS] = "Read-only file system",
  [EMLINK] = "Too many links",
  [EPIPE] = "Broken pipe",
  [ERANGE] = "Result out of range",
  [ENAMETOOLONG] = "File name too long",
  [ENOSYS] = "Function not implemented",
  [ENOTEMPTY] = "Directory not empty",
};

const char *strerror(int err) {
  if (err >= 0 && (size_t)err < sizeof(errnames) / sizeof(errnames[0]) && errnames[err])
    return errnames[err];
  return "Unknown error";
}

void perror(const char *msg) {
  if (msg && *msg) dprintf(2, "%s: %s\n", msg, strerror(errno));
  else dprintf(2, "%s\n", strerror(errno));
}

void die(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vdprintf(2, fmt, ap);
  va_end(ap);
  dprintf(2, "\n");
  exit(1);
}

// CRC-32 (IEEE 802.3, reflected), identical to zlib's crc32().
uint32_t crc32_update(uint32_t crc, const void *buf, size_t n) {
  static uint32_t table[256];
  if (!table[1]) {
    for (uint32_t i = 0; i < 256; i++) {
      uint32_t c = i;
      for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      table[i] = c;
    }
  }
  const uint8_t *p = buf;
  crc = ~crc;
  while (n--) crc = table[(crc ^ *p++) & 0xff] ^ (crc >> 8);
  return ~crc;
}
