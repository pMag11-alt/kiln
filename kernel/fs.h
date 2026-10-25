// ext2 on-disk structures (revision 1, little-endian) and the in-memory
// inode, buffer and open-file types built on top of them.
#pragma once

#include "proc.h"
#include "types.h"

#define EXT2_MAGIC       0xEF53
#define EXT2_ROOT_INO    2
#define EXT2_NDIR_BLOCKS 12
#define EXT2_IND_BLOCK   12
#define EXT2_DIND_BLOCK  13
#define EXT2_TIND_BLOCK  14
#define EXT2_N_BLOCKS    15

#define EXT2_FEATURE_INCOMPAT_FILETYPE 0x0002
#define EXT2_FEATURE_RO_COMPAT_SPARSE_SUPER 0x0001
#define EXT2_FEATURE_RO_COMPAT_LARGE_FILE   0x0002
#define EXT2_INDEX_FL    0x00001000   // hashed directory (we clear it on modification)

// Directory entry file types (incompat FILETYPE feature).
#define EXT2_FT_UNKNOWN  0
#define EXT2_FT_REG_FILE 1
#define EXT2_FT_DIR      2
#define EXT2_FT_CHRDEV   3
#define EXT2_FT_BLKDEV   4
#define EXT2_FT_FIFO     5
#define EXT2_FT_SOCK     6
#define EXT2_FT_SYMLINK  7

struct ext2_super {
  u32 s_inodes_count;
  u32 s_blocks_count;
  u32 s_r_blocks_count;
  u32 s_free_blocks_count;
  u32 s_free_inodes_count;
  u32 s_first_data_block;
  u32 s_log_block_size;
  u32 s_log_frag_size;
  u32 s_blocks_per_group;
  u32 s_frags_per_group;
  u32 s_inodes_per_group;
  u32 s_mtime;
  u32 s_wtime;
  u16 s_mnt_count;
  u16 s_max_mnt_count;
  u16 s_magic;
  u16 s_state;
  u16 s_errors;
  u16 s_minor_rev_level;
  u32 s_lastcheck;
  u32 s_checkinterval;
  u32 s_creator_os;
  u32 s_rev_level;
  u16 s_def_resuid;
  u16 s_def_resgid;
  // EXT2_DYNAMIC_REV
  u32 s_first_ino;
  u16 s_inode_size;
  u16 s_block_group_nr;
  u32 s_feature_compat;
  u32 s_feature_incompat;
  u32 s_feature_ro_compat;
  u8 s_uuid[16];
  char s_volume_name[16];
  char s_last_mounted[64];
  u32 s_algo_bitmap;
  u8 s_rest[1024 - 204];   // fields we never touch, preserved on write-back
} __packed;
_Static_assert(sizeof(struct ext2_super) == 1024, "superblock size");

struct ext2_group_desc {
  u32 bg_block_bitmap;
  u32 bg_inode_bitmap;
  u32 bg_inode_table;
  u16 bg_free_blocks_count;
  u16 bg_free_inodes_count;
  u16 bg_used_dirs_count;
  u16 bg_pad;
  u32 bg_reserved[3];
} __packed;
_Static_assert(sizeof(struct ext2_group_desc) == 32, "group descriptor size");

struct ext2_inode {
  u16 i_mode;
  u16 i_uid;
  u32 i_size;
  u32 i_atime;
  u32 i_ctime;
  u32 i_mtime;
  u32 i_dtime;
  u16 i_gid;
  u16 i_links_count;
  u32 i_blocks;         // 512-byte sectors, including indirect blocks
  u32 i_flags;
  u32 i_osd1;
  u32 i_block[EXT2_N_BLOCKS];
  u32 i_generation;
  u32 i_file_acl;
  u32 i_size_high;
  u32 i_faddr;
  u8 i_osd2[12];
} __packed;
_Static_assert(sizeof(struct ext2_inode) == 128, "inode size");

struct ext2_dirent {
  u32 inode;
  u16 rec_len;
  u8 name_len;
  u8 file_type;
  char name[];
} __packed;
#define EXT2_DIRENT_LEN(namelen) (((namelen) + 8 + 3) & ~3u)

// ---- buffer cache ----
struct buf {
  bool valid;           // data holds the block's contents
  bool disk;            // owned by the disk driver while I/O is in flight
  u32 blockno;
  u32 refcnt;
  struct sleeplock lock;
  struct buf *prev, *next;   // LRU list
  u8 data[BSIZE_MAX] __attribute__((aligned(16)));
};

// ---- in-memory inode ----
struct inode {
  u32 inum;
  int ref;              // protected by the inode-table spinlock
  struct sleeplock lock;
  bool valid;           // d has been read from disk
  struct ext2_inode d;
};

// ---- open files ----
enum ftype { FD_NONE, FD_PIPE, FD_INODE, FD_DEVICE };

struct file {
  enum ftype type;
  int ref;
  bool readable;
  bool writable;
  bool append;
  struct pipe *pipe;
  struct inode *ip;
  u64 off;              // FD_INODE
  u32 rdev;             // FD_DEVICE
  struct sleeplock poslock;   // serializes offset updates
};

struct devsw {
  i64 (*read)(int minor, u64 udst, u64 n);
  i64 (*write)(int minor, u64 usrc, u64 n);
};
extern struct devsw devsw[256];
