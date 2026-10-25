// The kiln user-space library: syscall wrappers plus a small libc.
#pragma once

#include "../../lib/klib.h"
#include <kiln/abi.h>

typedef long ssize_t;
typedef long off_t;
typedef int pid_t;

#define PATH_MAX 256
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

extern int errno;

// ---- system calls (return -1 and set errno on failure) ----
pid_t fork(void);   // flushes stdout first so buffered text is not duplicated
__attribute__((noreturn)) void exit(int status);    // flushes stdout
__attribute__((noreturn)) void _exit(int status);
pid_t waitpid(pid_t pid, int *status, int options);
pid_t wait(int *status);
int pipe(int fds[2]);
ssize_t read(int fd, void *buf, size_t n);
ssize_t write(int fd, const void *buf, size_t n);
int close(int fd);
int kill(pid_t pid, int sig);
int execve(const char *path, char *const argv[]);
int execv(const char *path, char *const argv[]);
int execvp(const char *file, char *const argv[]);
int fstat(int fd, struct stat *st);
int stat(const char *path, struct stat *st);
int chdir(const char *path);
int dup(int fd);
int dup2(int oldfd, int newfd);
pid_t getpid(void);
pid_t getppid(void);
void *sbrk(long increment);
int msleep(unsigned long ms);
unsigned sleep(unsigned seconds);
long uptime(void);           // milliseconds since boot
long time(long *t);          // seconds since the Unix epoch
int open(const char *path, int flags, ...);
int creat(const char *path, int mode);
int mknod(const char *path, int mode, int dev);
int unlink(const char *path);
int link(const char *old, const char *new);
int rename(const char *old, const char *new);
int mkdir(const char *path, int mode);
int rmdir(const char *path);
int getdents(int fd, void *buf, size_t n);
off_t lseek(int fd, off_t off, int whence);
int ftruncate(int fd, off_t len);
char *getcwd(char *buf, size_t size);
int procinfo(struct procinfo *buf, int max);
int sysinfo(struct sysinfo *si);
__attribute__((noreturn)) void shutdown(int how);
int setpgid(pid_t pid, pid_t pgid);
pid_t getpgid(pid_t pid);
int tcsetpgrp(pid_t pgid);

// Raw entry points (no errno translation).
long __sys_getcwd(char *buf, size_t size);
long __sys_time(void);

// ---- stdio-ish ----
int printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int dprintf(int fd, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int vdprintf(int fd, const char *fmt, va_list ap);
#define fprintf(fd, ...) dprintf(fd, __VA_ARGS__)
#define stderr STDERR_FILENO
#define stdout STDOUT_FILENO
int puts(const char *s);
int putchar(int c);
int fputs(const char *s, int fd);
void fflush(int fd);
void perror(const char *msg);
const char *strerror(int err);

// Buffered line reader over a file descriptor.
struct reader {
  int fd;
  int len, pos;
  bool eof;
  char buf[1024];
};
void reader_init(struct reader *r, int fd);
// Read one line (without the newline) into line[0..max). Returns its length,
// or -1 at end of file. Longer lines are split.
int reader_getline(struct reader *r, char *line, int max);

// ---- memory ----
void *malloc(size_t n);
void *calloc(size_t n, size_t size);
void *realloc(void *p, size_t n);
void free(void *p);
char *strdup(const char *s);

// ---- misc ----
int atoi(const char *s);
long strtol(const char *s, char **end, int base);
unsigned long strtoul(const char *s, char **end, int base);
int isatty(int fd);
int isdigit(int c);
int isspace(int c);
int isalpha(int c);
int isalnum(int c);
int isprint(int c);
int tolower(int c);
int toupper(int c);
void qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *));
const char *basename_of(const char *path);
__attribute__((noreturn)) void die(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
uint32_t crc32_update(uint32_t crc, const void *buf, size_t n);

// ---- directories ----
typedef struct {
  int fd;
  int len, pos;
  char buf[2048];
} DIR;
DIR *opendir(const char *path);
struct dirent *readdir(DIR *d);
int closedir(DIR *d);
