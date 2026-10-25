// The kernel/user ABI: syscall numbers, error codes, flags and the structs
// that cross the boundary. Shared verbatim by the kernel and user space.
#pragma once

// ---- syscall numbers (a7) ----
#define SYS_fork      1
#define SYS_exit      2
#define SYS_waitpid   3
#define SYS_pipe      4
#define SYS_read      5
#define SYS_write     6
#define SYS_close     7
#define SYS_kill      8
#define SYS_execve    9
#define SYS_fstat     10
#define SYS_chdir     11
#define SYS_dup       12
#define SYS_getpid    13
#define SYS_sbrk      14
#define SYS_msleep    15
#define SYS_uptime    16
#define SYS_open      17
#define SYS_mknod     18
#define SYS_unlink    19
#define SYS_link      20
#define SYS_mkdir     21
#define SYS_getdents  22
#define SYS_lseek     23
#define SYS_dup2      24
#define SYS_getppid   25
#define SYS_rename    26
#define SYS_rmdir     27
#define SYS_stat      28
#define SYS_getcwd    29
#define SYS_procinfo  30
#define SYS_sysinfo   31
#define SYS_shutdown  32
#define SYS_setpgid   33
#define SYS_getpgid   34
#define SYS_tcsetpgrp 35
#define SYS_time      36
#define SYS_ftruncate 37
#define NSYSCALL      38

// ---- errno values (same numbering as Linux) ----
#define EPERM        1
#define ENOENT       2
#define ESRCH        3
#define EINTR        4
#define EIO          5
#define E2BIG        7
#define ENOEXEC      8
#define EBADF        9
#define ECHILD       10
#define EAGAIN       11
#define ENOMEM       12
#define EACCES       13
#define EFAULT       14
#define EBUSY        16
#define EEXIST       17
#define EXDEV        18
#define ENODEV       19
#define ENOTDIR      20
#define EISDIR       21
#define EINVAL       22
#define ENFILE       23
#define EMFILE       24
#define ENOTTY       25
#define EFBIG        27
#define ENOSPC       28
#define ESPIPE       29
#define EROFS        30
#define EMLINK       31
#define EPIPE        32
#define ERANGE       34
#define ENAMETOOLONG 36
#define ENOSYS       38
#define ENOTEMPTY    39

// ---- open(2) flags ----
#define O_RDONLY    0x0000
#define O_WRONLY    0x0001
#define O_RDWR      0x0002
#define O_ACCMODE   0x0003
#define O_CREAT     0x0040
#define O_EXCL      0x0080
#define O_TRUNC     0x0200
#define O_APPEND    0x0400
#define O_DIRECTORY 0x10000

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

// ---- file modes ----
#define S_IFMT   0170000
#define S_IFSOCK 0140000
#define S_IFLNK  0120000
#define S_IFREG  0100000
#define S_IFBLK  0060000
#define S_IFDIR  0040000
#define S_IFCHR  0020000
#define S_IFIFO  0010000
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#define S_ISCHR(m) (((m) & S_IFMT) == S_IFCHR)
#define S_ISLNK(m) (((m) & S_IFMT) == S_IFLNK)
#define S_ISFIFO(m) (((m) & S_IFMT) == S_IFIFO)

// Character device numbers.
#define MKDEV(ma, mi) (((ma) << 8) | (mi))
#define DEV_MAJOR(d)  (((d) >> 8) & 0xff)
#define DEV_MINOR(d)  ((d) & 0xff)
#define MAJOR_CONSOLE 5
#define MAJOR_MEM     1   // minor 3 = null, 5 = zero (as on Linux)
#define MINOR_NULL    3
#define MINOR_ZERO    5

// ---- wait status ----
#define WNOHANG 1
#define WIFEXITED(s)   (((s) & 0x7f) == 0)
#define WEXITSTATUS(s) (((s) >> 8) & 0xff)
#define WIFSIGNALED(s) (((s) & 0x7f) != 0)
#define WTERMSIG(s)    ((s) & 0x7f)

// Signals only terminate (there are no handlers); the number is reported
// to the parent through the wait status.
#define SIGINT  2
#define SIGILL  4
#define SIGABRT 6
#define SIGBUS  7
#define SIGKILL 9
#define SIGSEGV 11
#define SIGPIPE 13
#define SIGTERM 15
#define NSIG    32

// shutdown(2)
#define SHUTDOWN_POWEROFF 0
#define SHUTDOWN_REBOOT   1

#ifndef __ASSEMBLER__
#include <stdint.h>

struct stat {
  uint32_t st_dev;
  uint32_t st_ino;
  uint32_t st_mode;
  uint32_t st_nlink;
  uint32_t st_uid;
  uint32_t st_gid;
  uint32_t st_rdev;
  uint32_t st_blocks;   // 512-byte units, as stored by ext2
  uint64_t st_size;
  int64_t st_atime;
  int64_t st_mtime;
  int64_t st_ctime;
};

// Records returned by getdents(2). d_reclen is 8-byte aligned.
#define DT_UNKNOWN 0
#define DT_FIFO    1
#define DT_CHR     2
#define DT_DIR     4
#define DT_BLK     6
#define DT_REG     8
#define DT_LNK     10
struct dirent {
  uint32_t d_ino;
  uint16_t d_reclen;
  uint8_t d_type;
  uint8_t d_namlen;
  char d_name[];        // NUL-terminated
};

#define PS_SLEEPING 1
#define PS_RUNNABLE 2
#define PS_RUNNING  3
#define PS_ZOMBIE   4
struct procinfo {
  int32_t pid;
  int32_t ppid;
  int32_t pgid;
  int32_t state;
  int32_t cpu;          // hart it is running on, or -1
  uint32_t pages;       // resident user pages (shared COW pages count once per process)
  uint64_t ticks;       // timer ticks spent running
  char name[16];
};

struct sysinfo {
  uint64_t uptime_ms;
  uint64_t total_pages;
  uint64_t free_pages;
  uint32_t nproc;
  uint32_t ncpu;
  uint64_t syscalls;
  uint64_t ctx_switches;
  uint64_t cow_faults;
  uint64_t lazy_faults;
  uint64_t disk_reads;
  uint64_t disk_writes;
  uint64_t bcache_hits;
  uint64_t bcache_misses;
  uint64_t fs_blocks;
  uint64_t fs_free_blocks;
  uint64_t fs_inodes;
  uint64_t fs_free_inodes;
  uint32_t fs_block_size;
  uint32_t pad;
};
#endif
