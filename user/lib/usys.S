# System call stubs. The kernel returns -errno on failure; the stubs turn
# that into -1 with errno set, as POSIX expects. The __sys_* variants return
# the raw value.
#include <kiln/abi.h>

.macro SYSCALL name, num
    .section .text.\name, "ax"
    .globl \name
\name:
    li      a7, \num
    ecall
    bltz    a0, 1f
    ret
1:  tail    __syscall_error
.endm

.macro RAWCALL name, num
    .section .text.\name, "ax"
    .globl \name
\name:
    li      a7, \num
    ecall
    ret
.endm

    .section .text.__syscall_error, "ax"
__syscall_error:
    neg     a0, a0
    lla     t0, errno
    sw      a0, 0(t0)
    li      a0, -1
    ret

SYSCALL __fork, SYS_fork
SYSCALL _exit, SYS_exit
SYSCALL waitpid, SYS_waitpid
SYSCALL pipe, SYS_pipe
SYSCALL read, SYS_read
SYSCALL write, SYS_write
SYSCALL close, SYS_close
SYSCALL kill, SYS_kill
SYSCALL execve, SYS_execve
SYSCALL fstat, SYS_fstat
SYSCALL chdir, SYS_chdir
SYSCALL dup, SYS_dup
SYSCALL getpid, SYS_getpid
SYSCALL __sbrk, SYS_sbrk
SYSCALL msleep, SYS_msleep
SYSCALL uptime, SYS_uptime
SYSCALL __open, SYS_open
SYSCALL mknod, SYS_mknod
SYSCALL unlink, SYS_unlink
SYSCALL link, SYS_link
SYSCALL mkdir, SYS_mkdir
SYSCALL getdents, SYS_getdents
SYSCALL lseek, SYS_lseek
SYSCALL dup2, SYS_dup2
SYSCALL getppid, SYS_getppid
SYSCALL rename, SYS_rename
SYSCALL rmdir, SYS_rmdir
SYSCALL stat, SYS_stat
SYSCALL procinfo, SYS_procinfo
SYSCALL sysinfo, SYS_sysinfo
SYSCALL shutdown, SYS_shutdown
SYSCALL setpgid, SYS_setpgid
SYSCALL getpgid, SYS_getpgid
SYSCALL tcsetpgrp, SYS_tcsetpgrp
SYSCALL ftruncate, SYS_ftruncate
RAWCALL __sys_getcwd, SYS_getcwd
RAWCALL __sys_time, SYS_time
