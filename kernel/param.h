#pragma once

#define NCPU          8      // maximum harts (hart ids must be < NCPU)
#define NPROC         64     // process table size
#define NOFILE        32     // open files per process
#define NFILE         256    // open files system-wide
#define NINODE        96     // in-memory inode cache
#define NBUF          96     // buffer cache blocks
#define MAXARG        32     // exec arguments
#define MAXPATH       256    // path length including NUL
#define NAME_MAX      255    // ext2 file name limit
#define BSIZE_MAX     4096   // largest supported file-system block
#define KSTACK_PAGES  4      // 16 KiB kernel stack per process
#define BOOT_STACK_SIZE 16384
#define TICK_HZ       100    // scheduler tick
#define USTACK_MAX    (1UL << 20)  // user stack may grow to 1 MiB
#define PIPESIZE      4096
