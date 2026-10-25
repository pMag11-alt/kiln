// Kernel-wide declarations.
#pragma once

#include "../lib/klib.h"
#include "fs.h"
#include "memlayout.h"
#include "proc.h"
#include "riscv.h"
#include "types.h"
#include <kiln/abi.h>

struct pipe;

// ---- fdt.c: what the device tree told us about the machine ----
#define MAX_VIRTIO 8
#define MAX_RESERVED 8
struct platform {
  u64 ram_base, ram_size;
  u64 uart_base;
  u32 uart_irq;
  u64 plic_base, plic_size;
  u64 rtc_base;
  u64 virtio_base[MAX_VIRTIO];
  u32 virtio_irq[MAX_VIRTIO];
  int nvirtio;
  u64 timebase;              // rdtime ticks per second
  u64 harts[NCPU];
  int nharts;
  u64 resv_base[MAX_RESERVED], resv_size[MAX_RESERVED];
  int nresv;
  char bootargs[128];
};
extern struct platform plat;
void fdt_parse(const void *fdt);

// ---- sbi.c ----
void sbi_set_timer(u64 stime);
long sbi_hart_start(u64 hartid, u64 start_pa, u64 opaque);
__noreturn void sbi_system_reset(u32 type, u32 reason);
long sbi_spec_version(void);

// ---- printf.c ----
void kprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
__noreturn void panic(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void backtrace(u64 fp);
extern volatile int panicking;
#define assert(x) \
  do { if (unlikely(!(x))) panic("assertion failed: %s (%s:%d)", #x, __FILE__, __LINE__); } while (0)

// ---- uart.c / console.c ----
void uart_init(void);
void uart_putc_sync(int c);
void uart_intr(void);
void console_init(void);
void console_intr(int c);
void console_set_fg(int pgid);

// ---- rtc.c ----
u64 rtc_now(void);   // seconds since the Unix epoch

// ---- spinlock.c / sleeplock.c ----
void initlock(struct spinlock *lk, const char *name);
void acquire(struct spinlock *lk);
void release(struct spinlock *lk);
bool holding(struct spinlock *lk);
void push_off(void);
void pop_off(void);
void initsleeplock(struct sleeplock *lk, const char *name);
void acquiresleep(struct sleeplock *lk);
void releasesleep(struct sleeplock *lk);
bool holdingsleep(struct sleeplock *lk);

// ---- kalloc.c ----
void kinit(void);
void *kalloc(void);
void *kzalloc(void);
void kfree(void *kva);
void kref_inc(void *kva);
int kref_get(void *kva);
u64 kmem_free_pages(void);
u64 kmem_total_pages(void);

// ---- vm.c ----
extern pagetable_t kernel_pagetable;
void kvminit(void);
void kvminithart(void);
pte_t *walk(pagetable_t pt, u64 va, bool alloc);
pagetable_t uvmcreate(void);
void uvmfree(pagetable_t pt);
int uvmcopy(pagetable_t old, pagetable_t new);
int uvm_map_page(pagetable_t pt, u64 va, void *kva, u64 perm);
void uvm_unmap_range(pagetable_t pt, u64 start, u64 end);
u32 uvm_count_pages(pagetable_t pt);
int handle_user_fault(struct proc *p, u64 va, u64 scause);
int copyout(struct proc *p, u64 dstva, const void *src, u64 len);
int copyin(struct proc *p, void *dst, u64 srcva, u64 len);
int copyinstr(struct proc *p, char *dst, u64 srcva, u64 max);
int copyout_pt(pagetable_t pt, u64 dstva, const void *src, u64 len);

// ---- proc.c ----
extern struct spinlock wait_lock;
void procinit(void);
void userinit(void);
struct cpu *mycpu(void);
struct proc *myproc(void);
int proc_fork(void);
__noreturn void proc_exit(int wstatus);
int proc_wait(int pid, u64 status_uaddr, int options);
int proc_kill(int pid, int sig);
int killed(struct proc *p);
void setkilled(struct proc *p, int sig);
void sleep(void *chan, struct spinlock *lk);
void wakeup(void *chan);
void yield(void);
void sched(void);
__noreturn void scheduler(void);
int proc_snapshot(struct procinfo *out, int max);
int proc_count(void);
void procdump(void);
int proc_setpgid(int pid, int pgid);
int proc_getpgid(int pid);

// ---- swtch / trap ----
void swtch(struct context *old, struct context *new);
void trap_vector(void);
__noreturn void userret(struct trapframe *tf);
void trapinithart(void);
void timer_inithart(void);
__noreturn void usertrapret(void);
extern volatile u64 ticks;
extern struct spinlock tickslock;
u64 uptime_ms(void);

// ---- plic.c ----
void plic_init(void);
void plic_inithart(void);
int plic_claim(void);
void plic_complete(int irq);

// ---- syscall.c ----
void syscall(void);
u64 argraw(int n);
int argstr(int n, char *buf, int max);

// ---- exec.c ----
int kexec(const char *path, char *const argv[]);

// ---- virtio_blk.c ----
void virtio_blk_init(void);
void virtio_blk_rw(struct buf *b, u32 bsize, bool write);
void virtio_blk_intr(void);
bool virtio_blk_owns_irq(int irq);

// ---- bio.c ----
void binit(void);
void bset_blocksize(u32 bsize);
u32 bblocksize(void);
struct buf *bread(u32 blockno);
struct buf *bget_zero(u32 blockno);
void bwrite(struct buf *b);
void brelse(struct buf *b);

// ---- ext2.c ----
void fs_mount(void);
struct inode *iget(u32 inum);
struct inode *idup(struct inode *ip);
void ilock(struct inode *ip);
void iunlock(struct inode *ip);
void iput(struct inode *ip);
void iunlockput(struct inode *ip);
void iupdate(struct inode *ip);
int itrunc(struct inode *ip, u64 size);
i64 readi(struct inode *ip, bool user, u64 dst, u64 off, u64 n);
i64 writei(struct inode *ip, bool user, u64 src, u64 off, u64 n);
void stati(struct inode *ip, struct stat *st);
int namei(const char *path, struct inode **out);
int nameiparent(const char *path, char *name, struct inode **out);
struct inode *dirlookup(struct inode *dp, const char *name, u32 *poff);
int dirlink(struct inode *dp, const char *name, u32 inum, u16 mode);
int dirunlink(struct inode *dp, u32 off);
int dir_isempty(struct inode *dp);
int dir_set_dotdot(struct inode *dp, u32 inum);
int dir_set_entry(struct inode *dp, u32 off, u32 inum, u16 mode);
int ext2_create(struct inode *dp, const char *name, u16 mode, u32 rdev, struct inode **out);
i64 dir_getdents(struct inode *dp, u64 *off, u64 udst, u64 n);
int fs_getcwd(struct inode *cwd, char *buf, int size);
void fs_statfs(u64 *blocks, u64 *bfree, u32 *bsize, u64 *inodes, u64 *ifree);
extern struct sleeplock ns_lock;
static inline bool ext2_isdir(struct inode *ip) { return S_ISDIR(ip->d.i_mode); }

// ---- file.c / pipe.c ----
void fileinit(void);
struct file *filealloc(void);
struct file *filedup(struct file *f);
void fileclose(struct file *f);
i64 fileread(struct file *f, u64 dst, u64 n);
i64 filewrite(struct file *f, u64 src, u64 n);
int filestat(struct file *f, u64 dst);
int pipealloc(struct file **f0, struct file **f1);
void pipeclose(struct pipe *pi, bool writable);
i64 piperead(struct pipe *pi, u64 dst, u64 n);
i64 pipewrite(struct pipe *pi, u64 src, u64 n);

// ---- statistics (bumped with relaxed atomics) ----
struct kstats {
  u64 syscalls, ctx_switches, cow_faults, lazy_faults;
  u64 disk_reads, disk_writes, bcache_hits, bcache_misses;
};
extern struct kstats kstats;
#define STAT_INC(field) __atomic_fetch_add(&kstats.field, 1, __ATOMIC_RELAXED)
