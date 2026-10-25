// ext2 (revision 1) read/write driver.
//
// The root file system is a standard ext2 image built by mke2fs, and what
// the kernel writes stays readable by Linux tools: the test suite checks the
// image with e2fsck after every run. Supported: 1/2/4 KiB blocks, direct,
// single, double and triple indirect blocks, sparse files, the FILETYPE
// directory feature, hard links and character devices. Not supported:
// journaling (ext3), extents (ext4), htree indexes (cleared on write) and
// extended attributes.
#include "defs.h"

static struct {
  struct sleeplock lock;   // bitmaps, group descriptors, free counts
  struct ext2_super sb;
  struct ext2_group_desc *gd;
  u32 bsize;
  u32 apb;                 // block addresses per indirect block
  u32 ngroups;
  u32 ipg, bpg;
  u32 inode_size;
  u32 first_ino;
  u32 first_data_block;
  u32 nblocks;
  u32 gdt_block;
  bool filetype;
} fs;

// Serializes operations that change the namespace (create, link, unlink,
// rename, mkdir, rmdir). Lookups never take it. Because each modifier holds
// it, a modifier may lock two directories at once without deadlock: path
// walks hold at most one inode lock at a time.
struct sleeplock ns_lock;

static struct {
  struct spinlock lock;
  struct inode inode[NINODE];
} itable;

static const u8 zeros[BSIZE_MAX];

static u32 now(void) {
  return (u32)rtc_now();
}

// ---- superblock and group descriptors ----

static void write_super(void) {
  u32 blk = fs.bsize == 1024 ? 1 : 0;
  u32 off = fs.bsize == 1024 ? 0 : 1024;
  fs.sb.s_wtime = now();
  struct buf *b = bread(blk);
  memcpy(b->data + off, &fs.sb, sizeof(fs.sb));
  bwrite(b);
  brelse(b);
}

static void write_gd(u32 g) {
  u32 per = fs.bsize / sizeof(struct ext2_group_desc);
  struct buf *b = bread(fs.gdt_block + g / per);
  memcpy(b->data + (g % per) * sizeof(struct ext2_group_desc), &fs.gd[g], sizeof(fs.gd[g]));
  bwrite(b);
  brelse(b);
}

void fs_mount(void) {
  initsleeplock(&fs.lock, "ext2");
  initsleeplock(&ns_lock, "namespace");
  initlock(&itable.lock, "itable");
  for (int i = 0; i < NINODE; i++) initsleeplock(&itable.inode[i].lock, "inode");

  // The superblock always lives at byte 1024, i.e. block 1 in 1 KiB units.
  bset_blocksize(1024);
  struct buf *b = bread(1);
  memcpy(&fs.sb, b->data, sizeof(fs.sb));
  brelse(b);

  struct ext2_super *sb = &fs.sb;
  if (sb->s_magic != EXT2_MAGIC) panic("ext2: bad magic %#x", sb->s_magic);
  u32 incompat = sb->s_rev_level >= 1 ? sb->s_feature_incompat : 0;
  u32 ro_compat = sb->s_rev_level >= 1 ? sb->s_feature_ro_compat : 0;
  if (incompat & ~EXT2_FEATURE_INCOMPAT_FILETYPE)
    panic("ext2: unsupported incompatible features %#x (need plain ext2)", incompat);
  if (ro_compat & ~(EXT2_FEATURE_RO_COMPAT_SPARSE_SUPER | EXT2_FEATURE_RO_COMPAT_LARGE_FILE))
    panic("ext2: unsupported ro-compat features %#x", ro_compat);

  fs.bsize = 1024u << sb->s_log_block_size;
  if (fs.bsize > BSIZE_MAX) panic("ext2: block size %u too large", fs.bsize);
  fs.apb = fs.bsize / 4;
  fs.ipg = sb->s_inodes_per_group;
  fs.bpg = sb->s_blocks_per_group;
  fs.nblocks = sb->s_blocks_count;
  fs.first_data_block = sb->s_first_data_block;
  fs.ngroups = (fs.nblocks - fs.first_data_block + fs.bpg - 1) / fs.bpg;
  fs.inode_size = sb->s_rev_level >= 1 ? sb->s_inode_size : 128;
  fs.first_ino = sb->s_rev_level >= 1 ? sb->s_first_ino : 11;
  fs.filetype = (incompat & EXT2_FEATURE_INCOMPAT_FILETYPE) != 0;
  fs.gdt_block = fs.first_data_block + 1;
  bset_blocksize(fs.bsize);

  if (fs.ngroups * sizeof(struct ext2_group_desc) > PGSIZE) panic("ext2: too many groups");
  fs.gd = kzalloc();
  if (!fs.gd) panic("ext2: out of memory");
  u32 gd_bytes = fs.ngroups * sizeof(struct ext2_group_desc);
  for (u32 off = 0; off < gd_bytes; off += fs.bsize) {
    b = bread(fs.gdt_block + off / fs.bsize);
    memcpy((u8 *)fs.gd + off, b->data, MIN(fs.bsize, gd_bytes - off));
    brelse(b);
  }

  sb->s_mnt_count++;
  sb->s_mtime = now();
  write_super();
  kprintf("ext2: %u x %u-byte blocks (%u free), %u inodes (%u free), %u groups\n",
          fs.nblocks, fs.bsize, sb->s_free_blocks_count, sb->s_inodes_count,
          sb->s_free_inodes_count, fs.ngroups);
}

// ---- bitmaps ----

static int find_zero_bit(const u8 *map, u32 nbits, u32 start) {
  for (u32 i = start; i < nbits; i++) {
    if (map[i / 8] == 0xff) {
      i |= 7;
      continue;
    }
    if (!(map[i / 8] & (1u << (i % 8)))) return (int)i;
  }
  return -1;
}

static u32 blocks_in_group(u32 g) {
  if (g + 1 < fs.ngroups) return fs.bpg;
  return fs.nblocks - fs.first_data_block - g * fs.bpg;
}

static u32 balloc(u32 goal_group) {
  acquiresleep(&fs.lock);
  for (u32 k = 0; k < fs.ngroups; k++) {
    u32 g = (goal_group + k) % fs.ngroups;
    if (fs.gd[g].bg_free_blocks_count == 0) continue;
    struct buf *b = bread(fs.gd[g].bg_block_bitmap);
    int bit = find_zero_bit(b->data, blocks_in_group(g), 0);
    if (bit < 0) {
      brelse(b);
      continue;
    }
    b->data[bit / 8] |= (u8)(1u << (bit % 8));
    bwrite(b);
    brelse(b);
    fs.gd[g].bg_free_blocks_count--;
    write_gd(g);
    fs.sb.s_free_blocks_count--;
    write_super();
    releasesleep(&fs.lock);
    return fs.first_data_block + g * fs.bpg + (u32)bit;
  }
  releasesleep(&fs.lock);
  return 0;
}

static void bfree(u32 blk) {
  if (blk < fs.first_data_block || blk >= fs.nblocks) panic("bfree: block %u out of range", blk);
  u32 g = (blk - fs.first_data_block) / fs.bpg;
  u32 bit = (blk - fs.first_data_block) % fs.bpg;
  acquiresleep(&fs.lock);
  struct buf *b = bread(fs.gd[g].bg_block_bitmap);
  if (!(b->data[bit / 8] & (1u << (bit % 8)))) panic("bfree: block %u already free", blk);
  b->data[bit / 8] &= (u8)~(1u << (bit % 8));
  bwrite(b);
  brelse(b);
  fs.gd[g].bg_free_blocks_count++;
  write_gd(g);
  fs.sb.s_free_blocks_count++;
  write_super();
  releasesleep(&fs.lock);
}

static u32 ialloc(u32 goal_group, bool dir) {
  acquiresleep(&fs.lock);
  for (u32 k = 0; k < fs.ngroups; k++) {
    u32 g = (goal_group + k) % fs.ngroups;
    if (fs.gd[g].bg_free_inodes_count == 0) continue;
    struct buf *b = bread(fs.gd[g].bg_inode_bitmap);
    // Inodes 1 .. first_ino-1 are reserved (2 is the root).
    int bit = find_zero_bit(b->data, fs.ipg, g == 0 ? fs.first_ino - 1 : 0);
    if (bit < 0) {
      brelse(b);
      continue;
    }
    b->data[bit / 8] |= (u8)(1u << (bit % 8));
    bwrite(b);
    brelse(b);
    fs.gd[g].bg_free_inodes_count--;
    if (dir) fs.gd[g].bg_used_dirs_count++;
    write_gd(g);
    fs.sb.s_free_inodes_count--;
    write_super();
    releasesleep(&fs.lock);
    return g * fs.ipg + (u32)bit + 1;
  }
  releasesleep(&fs.lock);
  return 0;
}

static void ifree(u32 inum, bool dir) {
  u32 g = (inum - 1) / fs.ipg;
  u32 bit = (inum - 1) % fs.ipg;
  acquiresleep(&fs.lock);
  struct buf *b = bread(fs.gd[g].bg_inode_bitmap);
  if (!(b->data[bit / 8] & (1u << (bit % 8)))) panic("ifree: inode %u already free", inum);
  b->data[bit / 8] &= (u8)~(1u << (bit % 8));
  bwrite(b);
  brelse(b);
  fs.gd[g].bg_free_inodes_count++;
  if (dir) fs.gd[g].bg_used_dirs_count--;
  write_gd(g);
  fs.sb.s_free_inodes_count++;
  write_super();
  releasesleep(&fs.lock);
}

// New directories go to the group with the most free inodes, spreading the
// tree out; files stay near their directory.
static u32 dir_group(void) {
  u32 best = 0;
  for (u32 g = 1; g < fs.ngroups; g++)
    if (fs.gd[g].bg_free_inodes_count > fs.gd[best].bg_free_inodes_count) best = g;
  return best;
}

// ---- inodes ----

static void inode_location(u32 inum, u32 *blk, u32 *off) {
  u32 g = (inum - 1) / fs.ipg;
  u64 byte = (u64)((inum - 1) % fs.ipg) * fs.inode_size;
  *blk = fs.gd[g].bg_inode_table + (u32)(byte / fs.bsize);
  *off = (u32)(byte % fs.bsize);
}

// Write ip->d back. Bytes beyond the 128-byte core of larger on-disk
// inodes are left as they are.
void iupdate(struct inode *ip) {
  u32 blk, off;
  inode_location(ip->inum, &blk, &off);
  struct buf *b = bread(blk);
  memcpy(b->data + off, &ip->d, sizeof(ip->d));
  bwrite(b);
  brelse(b);
}

struct inode *iget(u32 inum) {
  if (inum == 0 || inum > fs.sb.s_inodes_count) panic("iget: bad inode number %u", inum);
  acquire(&itable.lock);
  struct inode *empty = NULL;
  for (struct inode *ip = itable.inode; ip < itable.inode + NINODE; ip++) {
    if (ip->ref > 0 && ip->inum == inum) {
      ip->ref++;
      release(&itable.lock);
      return ip;
    }
    if (!empty && ip->ref == 0) empty = ip;
  }
  if (!empty) panic("iget: inode table full");
  empty->inum = inum;
  empty->ref = 1;
  empty->valid = false;
  release(&itable.lock);
  return empty;
}

struct inode *idup(struct inode *ip) {
  acquire(&itable.lock);
  ip->ref++;
  release(&itable.lock);
  return ip;
}

void ilock(struct inode *ip) {
  if (!ip || ip->ref < 1) panic("ilock: bad inode");
  acquiresleep(&ip->lock);
  if (!ip->valid) {
    u32 blk, off;
    inode_location(ip->inum, &blk, &off);
    struct buf *b = bread(blk);
    memcpy(&ip->d, b->data + off, sizeof(ip->d));
    brelse(b);
    ip->valid = true;
    if (ip->d.i_mode == 0) panic("ilock: inode %u is not allocated", ip->inum);
  }
}

void iunlock(struct inode *ip) {
  if (!ip || !holdingsleep(&ip->lock) || ip->ref < 1) panic("iunlock: bad inode");
  releasesleep(&ip->lock);
}

// Drop a reference. When the last reference to an unlinked inode goes
// away, its blocks and the inode itself are freed.
void iput(struct inode *ip) {
  acquire(&itable.lock);
  if (ip->ref == 1 && ip->valid && ip->d.i_links_count == 0) {
    // ref == 1 means nobody else can be holding the lock.
    acquiresleep(&ip->lock);
    release(&itable.lock);
    bool dir = S_ISDIR(ip->d.i_mode);
    // Device inodes keep their device number in i_block[0]: not a block.
    if (S_ISREG(ip->d.i_mode) || S_ISDIR(ip->d.i_mode)) itrunc(ip, 0);
    ip->d.i_dtime = now();
    iupdate(ip);
    ifree(ip->inum, dir);
    ip->valid = false;
    releasesleep(&ip->lock);
    acquire(&itable.lock);
  }
  ip->ref--;
  release(&itable.lock);
}

void iunlockput(struct inode *ip) {
  iunlock(ip);
  iput(ip);
}

// ---- block mapping ----

// balloc() for an inode, accounting the block in i_blocks (512-byte units).
static u32 inode_balloc(struct inode *ip, bool zero) {
  u32 blk = balloc((ip->inum - 1) / fs.ipg);
  if (!blk) return 0;
  if (zero) {
    struct buf *b = bget_zero(blk);
    bwrite(b);
    brelse(b);
  }
  ip->d.i_blocks += fs.bsize / 512;
  return blk;
}

// Map logical block lbn of ip to a disk block, 0 for a hole. With alloc,
// missing data and indirect blocks are allocated and *fresh says whether
// the data block is new (its on-disk contents are garbage). The caller
// writes the inode back.
static int bmap(struct inode *ip, u32 lbn, bool alloc, u32 *out, bool *fresh) {
  u32 apb = fs.apb;
  u32 idx[3];
  int depth;
  u32 *slot;
  if (fresh) *fresh = false;

  if (lbn < EXT2_NDIR_BLOCKS) {
    slot = &ip->d.i_block[lbn];
    depth = 0;
  } else if ((lbn -= EXT2_NDIR_BLOCKS) < apb) {
    slot = &ip->d.i_block[EXT2_IND_BLOCK];
    depth = 1;
    idx[0] = lbn;
  } else if ((lbn -= apb) < apb * apb) {
    slot = &ip->d.i_block[EXT2_DIND_BLOCK];
    depth = 2;
    idx[0] = lbn / apb;
    idx[1] = lbn % apb;
  } else if ((u64)(lbn -= apb * apb) < (u64)apb * apb * apb) {
    slot = &ip->d.i_block[EXT2_TIND_BLOCK];
    depth = 3;
    idx[0] = lbn / (apb * apb);
    idx[1] = (lbn / apb) % apb;
    idx[2] = lbn % apb;
  } else {
    return -EFBIG;
  }

  u32 blk = *slot;
  if (!blk) {
    if (!alloc) {
      *out = 0;
      return 0;
    }
    blk = inode_balloc(ip, depth > 0);   // indirect blocks must start zeroed
    if (!blk) return -ENOSPC;
    *slot = blk;
    if (depth == 0 && fresh) *fresh = true;
  }
  for (int level = 0; level < depth; level++) {
    struct buf *b = bread(blk);
    u32 *entries = (u32 *)b->data;
    u32 next = entries[idx[level]];
    if (!next) {
      if (!alloc) {
        brelse(b);
        *out = 0;
        return 0;
      }
      bool data = level == depth - 1;
      next = inode_balloc(ip, !data);
      if (!next) {
        brelse(b);
        return -ENOSPC;
      }
      entries[idx[level]] = next;
      bwrite(b);
      if (data && fresh) *fresh = true;
    }
    brelse(b);
    blk = next;
  }
  *out = blk;
  return 0;
}

// Free the subtree under blk (an indirect block of the given depth, or a
// data block at depth 0) for logical blocks >= keep. base is the first
// logical block the subtree covers. Returns true if blk itself was freed.
static bool trunc_tree(struct inode *ip, u32 blk, int depth, u64 base, u64 keep) {
  u64 span = 1;
  for (int i = 0; i < depth; i++) span *= fs.apb;
  if (base >= keep) {
    if (depth > 0) {
      struct buf *b = bread(blk);
      u32 *entries = (u32 *)b->data;
      for (u32 i = 0; i < fs.apb; i++)
        if (entries[i]) trunc_tree(ip, entries[i], depth - 1, 0, 0);
      brelse(b);
    }
    bfree(blk);
    ip->d.i_blocks -= fs.bsize / 512;
    return true;
  }
  if (depth == 0 || base + span <= keep) return false;
  u64 child_span = span / fs.apb;
  struct buf *b = bread(blk);
  u32 *entries = (u32 *)b->data;
  bool dirty = false;
  for (u32 i = 0; i < fs.apb; i++) {
    u64 cbase = base + i * child_span;
    if (!entries[i] || cbase + child_span <= keep) continue;
    if (trunc_tree(ip, entries[i], depth - 1, cbase, keep)) {
      entries[i] = 0;
      dirty = true;
    }
  }
  if (dirty) bwrite(b);
  brelse(b);
  return false;
}

// Set the file size, freeing blocks past the new end. Growing just moves
// the size (the gap reads as zeros).
int itrunc(struct inode *ip, u64 size) {
  if (size > 0x7fffffff) return -EFBIG;
  u64 old = ip->d.i_size;
  if (size < old) {
    u64 keep = (size + fs.bsize - 1) / fs.bsize;
    u64 apb = fs.apb;
    for (u32 i = 0; i < EXT2_NDIR_BLOCKS; i++) {
      if (ip->d.i_block[i] && i >= keep) {
        bfree(ip->d.i_block[i]);
        ip->d.i_blocks -= fs.bsize / 512;
        ip->d.i_block[i] = 0;
      }
    }
    u64 bases[3] = {EXT2_NDIR_BLOCKS, EXT2_NDIR_BLOCKS + apb, EXT2_NDIR_BLOCKS + apb + apb * apb};
    for (int d = 0; d < 3; d++) {
      u32 *slot = &ip->d.i_block[EXT2_IND_BLOCK + d];
      if (*slot && trunc_tree(ip, *slot, d + 1, bases[d], keep)) *slot = 0;
    }
    // Zero the tail of a partial last block so a later extension reads 0s.
    if (size % fs.bsize) {
      u32 blk;
      if (bmap(ip, (u32)(size / fs.bsize), false, &blk, NULL) == 0 && blk) {
        struct buf *b = bread(blk);
        memset(b->data + size % fs.bsize, 0, fs.bsize - size % fs.bsize);
        bwrite(b);
        brelse(b);
      }
    }
  }
  ip->d.i_size = (u32)size;
  ip->d.i_mtime = ip->d.i_ctime = now();
  iupdate(ip);
  return 0;
}

static int copy_to(bool user, u64 dst, const void *src, u64 n) {
  if (user) return copyout(myproc(), dst, src, n);
  memmove((void *)dst, src, n);
  return 0;
}

static int copy_from(bool user, void *dst, u64 src, u64 n) {
  if (user) return copyin(myproc(), dst, src, n);
  memmove(dst, (const void *)src, n);
  return 0;
}

i64 readi(struct inode *ip, bool user, u64 dst, u64 off, u64 n) {
  u64 size = ip->d.i_size;
  if (off >= size) return 0;
  if (n > size - off) n = size - off;
  u64 done = 0;
  while (done < n) {
    u32 boff = (u32)(off % fs.bsize);
    u64 m = MIN(n - done, (u64)(fs.bsize - boff));
    u32 blk;
    int r = bmap(ip, (u32)(off / fs.bsize), false, &blk, NULL);
    if (r < 0) return done ? (i64)done : r;
    if (blk == 0) {
      r = copy_to(user, dst, zeros, m);   // a hole
    } else {
      struct buf *b = bread(blk);
      r = copy_to(user, dst, b->data + boff, m);
      brelse(b);
    }
    if (r < 0) return done ? (i64)done : r;
    done += m;
    off += m;
    dst += m;
  }
  return (i64)done;
}

// Write n bytes at off, allocating blocks as needed. User data is staged in
// a bounce page first, so a bad user pointer never leaves a block allocated
// past the end of the file.
i64 writei(struct inode *ip, bool user, u64 src, u64 off, u64 n) {
  if (off + n < off || off + n > 0x7fffffff) return -EFBIG;
  u8 *bounce = kalloc();
  if (!bounce) return -ENOMEM;
  u64 done = 0;
  int err = 0;
  while (done < n) {
    u32 boff = (u32)(off % fs.bsize);
    u64 m = MIN(n - done, (u64)(fs.bsize - boff));
    if ((err = copy_from(user, bounce, src, m)) < 0) break;
    u32 blk;
    bool fresh;
    if ((err = bmap(ip, (u32)(off / fs.bsize), true, &blk, &fresh)) < 0) break;
    // A new block, or one we overwrite entirely, needs no read.
    struct buf *b = (fresh || m == fs.bsize) ? bget_zero(blk) : bread(blk);
    memcpy(b->data + boff, bounce, m);
    bwrite(b);
    brelse(b);
    done += m;
    off += m;
    src += m;
  }
  kfree(bounce);
  if (off > ip->d.i_size) ip->d.i_size = (u32)off;
  if (done) ip->d.i_mtime = ip->d.i_ctime = now();
  iupdate(ip);
  return done ? (i64)done : err;
}

void stati(struct inode *ip, struct stat *st) {
  memset(st, 0, sizeof(*st));
  st->st_dev = 1;
  st->st_ino = ip->inum;
  st->st_mode = ip->d.i_mode;
  st->st_nlink = ip->d.i_links_count;
  st->st_uid = ip->d.i_uid;
  st->st_gid = ip->d.i_gid;
  st->st_blocks = ip->d.i_blocks;
  st->st_size = ip->d.i_size;
  st->st_atime = ip->d.i_atime;
  st->st_mtime = ip->d.i_mtime;
  st->st_ctime = ip->d.i_ctime;
  if (S_ISCHR(ip->d.i_mode) || (ip->d.i_mode & S_IFMT) == S_IFBLK) {
    u32 dev = ip->d.i_block[0];
    if (dev) {
      st->st_rdev = MKDEV((dev >> 8) & 0xff, dev & 0xff);
    } else {
      dev = ip->d.i_block[1];   // "new" Linux encoding
      st->st_rdev = MKDEV((dev & 0xfff00) >> 8, (dev & 0xff) | ((dev >> 12) & 0xfff00));
    }
  }
}

// ---- directories ----

static u8 mode_to_ftype(u16 mode) {
  switch (mode & S_IFMT) {
  case S_IFREG: return EXT2_FT_REG_FILE;
  case S_IFDIR: return EXT2_FT_DIR;
  case S_IFCHR: return EXT2_FT_CHRDEV;
  case S_IFBLK: return EXT2_FT_BLKDEV;
  case S_IFIFO: return EXT2_FT_FIFO;
  case S_IFSOCK: return EXT2_FT_SOCK;
  case S_IFLNK: return EXT2_FT_SYMLINK;
  }
  return EXT2_FT_UNKNOWN;
}

static u8 ftype_to_dt(u8 ft) {
  static const u8 map[] = {DT_UNKNOWN, DT_REG, DT_DIR, DT_CHR, DT_BLK, DT_FIFO, 12, DT_LNK};
  return ft < sizeof(map) ? map[ft] : DT_UNKNOWN;
}

static bool dirent_ok(const struct ext2_dirent *de, u32 pos) {
  return de->rec_len >= 8 && (de->rec_len & 3) == 0 && pos + de->rec_len <= fs.bsize &&
         EXT2_DIRENT_LEN(de->name_len) <= de->rec_len;
}

static void dir_touch(struct inode *dp) {
  dp->d.i_mtime = dp->d.i_ctime = now();
  dp->d.i_flags &= ~EXT2_INDEX_FL;   // we do not maintain htree indexes
  iupdate(dp);
}

// Read block lbn of directory dp. NULL means a hole (should not happen).
static struct buf *dir_block(struct inode *dp, u32 lbn) {
  u32 blk;
  if (bmap(dp, lbn, false, &blk, NULL) < 0 || !blk) return NULL;
  return bread(blk);
}

// Look for name in dp (locked). Returns an unlocked, referenced inode and
// the entry's byte offset in *poff.
struct inode *dirlookup(struct inode *dp, const char *name, u32 *poff) {
  if (!S_ISDIR(dp->d.i_mode)) panic("dirlookup: not a directory");
  size_t nlen = strlen(name);
  for (u32 lbn = 0; lbn < dp->d.i_size / fs.bsize; lbn++) {
    struct buf *b = dir_block(dp, lbn);
    if (!b) continue;
    for (u32 pos = 0; pos < fs.bsize;) {
      struct ext2_dirent *de = (struct ext2_dirent *)(b->data + pos);
      if (!dirent_ok(de, pos)) {
        kprintf("ext2: corrupt directory %u block %u\n", dp->inum, lbn);
        break;
      }
      if (de->inode && de->name_len == nlen && memcmp(de->name, name, nlen) == 0) {
        u32 inum = de->inode;
        if (poff) *poff = lbn * fs.bsize + pos;
        brelse(b);
        return iget(inum);
      }
      pos += de->rec_len;
    }
    brelse(b);
  }
  return NULL;
}

// Add an entry name -> inum. Reuses slack after an existing entry or an
// empty slot; otherwise appends a block.
int dirlink(struct inode *dp, const char *name, u32 inum, u16 mode) {
  size_t nlen = strlen(name);
  if (nlen == 0) return -ENOENT;
  if (nlen > NAME_MAX) return -ENAMETOOLONG;
  u16 need = (u16)EXT2_DIRENT_LEN(nlen);
  u8 ftype = fs.filetype ? mode_to_ftype(mode) : 0;

  for (u32 lbn = 0; lbn < dp->d.i_size / fs.bsize; lbn++) {
    struct buf *b = dir_block(dp, lbn);
    if (!b) continue;
    for (u32 pos = 0; pos < fs.bsize;) {
      struct ext2_dirent *de = (struct ext2_dirent *)(b->data + pos);
      if (!dirent_ok(de, pos)) break;
      u16 used = de->inode ? (u16)EXT2_DIRENT_LEN(de->name_len) : 0;
      if (de->rec_len - used >= need) {
        struct ext2_dirent *ne = de;
        if (used) {
          ne = (struct ext2_dirent *)((u8 *)de + used);
          ne->rec_len = de->rec_len - used;
          de->rec_len = used;
        }
        ne->inode = inum;
        ne->name_len = (u8)nlen;
        ne->file_type = ftype;
        memcpy(ne->name, name, nlen);
        bwrite(b);
        brelse(b);
        dir_touch(dp);
        return 0;
      }
      pos += de->rec_len;
    }
    brelse(b);
  }

  u32 blk;
  bool fresh;
  int r = bmap(dp, dp->d.i_size / fs.bsize, true, &blk, &fresh);
  if (r < 0) {
    iupdate(dp);
    return r;
  }
  struct buf *b = bget_zero(blk);
  struct ext2_dirent *ne = (struct ext2_dirent *)b->data;
  ne->inode = inum;
  ne->rec_len = (u16)fs.bsize;
  ne->name_len = (u8)nlen;
  ne->file_type = ftype;
  memcpy(ne->name, name, nlen);
  bwrite(b);
  brelse(b);
  dp->d.i_size += fs.bsize;
  dir_touch(dp);
  return 0;
}

// Remove the entry at byte offset off (from dirlookup) by folding it into
// its predecessor, or clearing its inode if it starts the block.
int dirunlink(struct inode *dp, u32 off) {
  u32 boff = off % fs.bsize;
  struct buf *b = dir_block(dp, off / fs.bsize);
  if (!b) return -EIO;
  u32 pos = 0, prev = UINT32_MAX;
  while (pos < boff) {
    struct ext2_dirent *de = (struct ext2_dirent *)(b->data + pos);
    if (!dirent_ok(de, pos)) break;
    prev = pos;
    pos += de->rec_len;
  }
  if (pos != boff) {
    brelse(b);
    return -EIO;
  }
  struct ext2_dirent *de = (struct ext2_dirent *)(b->data + boff);
  if (prev != UINT32_MAX) ((struct ext2_dirent *)(b->data + prev))->rec_len += de->rec_len;
  de->inode = 0;
  bwrite(b);
  brelse(b);
  dir_touch(dp);
  return 0;
}

static bool is_dot(const struct ext2_dirent *de) {
  return (de->name_len == 1 && de->name[0] == '.') ||
         (de->name_len == 2 && de->name[0] == '.' && de->name[1] == '.');
}

int dir_isempty(struct inode *dp) {
  for (u32 lbn = 0; lbn < dp->d.i_size / fs.bsize; lbn++) {
    struct buf *b = dir_block(dp, lbn);
    if (!b) continue;
    for (u32 pos = 0; pos < fs.bsize;) {
      struct ext2_dirent *de = (struct ext2_dirent *)(b->data + pos);
      if (!dirent_ok(de, pos)) break;
      if (de->inode && !is_dot(de)) {
        brelse(b);
        return 0;
      }
      pos += de->rec_len;
    }
    brelse(b);
  }
  return 1;
}

// Point the entry at byte offset off in dp (locked) at another inode.
int dir_set_entry(struct inode *dp, u32 off, u32 inum, u16 mode) {
  struct buf *b = dir_block(dp, off / fs.bsize);
  if (!b) return -EIO;
  struct ext2_dirent *de = (struct ext2_dirent *)(b->data + off % fs.bsize);
  de->inode = inum;
  if (fs.filetype) de->file_type = mode_to_ftype(mode);
  bwrite(b);
  brelse(b);
  dir_touch(dp);
  return 0;
}

// Repoint dp's ".." entry (used when rename moves a directory).
int dir_set_dotdot(struct inode *dp, u32 inum) {
  u32 off;
  struct inode *old = dirlookup(dp, "..", &off);
  if (!old) return -EIO;
  iput(old);
  return dir_set_entry(dp, off, inum, S_IFDIR);
}

// Create name in dp (locked; ns_lock held) with the given mode. On success
// *out is the new inode, locked.
int ext2_create(struct inode *dp, const char *name, u16 mode, u32 rdev, struct inode **out) {
  bool dir = S_ISDIR(mode);
  u32 inum = ialloc(dir ? dir_group() : (dp->inum - 1) / fs.ipg, dir);
  if (!inum) return -ENOSPC;

  struct inode *ip = iget(inum);
  acquiresleep(&ip->lock);
  // Fresh inode: initialize instead of reading stale contents. Clear the
  // whole on-disk slot so larger inodes carry no leftover extra fields.
  {
    u32 blk, off;
    inode_location(inum, &blk, &off);
    struct buf *b = bread(blk);
    memset(b->data + off, 0, fs.inode_size);
    bwrite(b);
    brelse(b);
  }
  memset(&ip->d, 0, sizeof(ip->d));
  ip->valid = true;
  u32 t = now();
  ip->d.i_mode = mode;
  ip->d.i_links_count = dir ? 2 : 1;
  ip->d.i_atime = ip->d.i_ctime = ip->d.i_mtime = t;
  if (S_ISCHR(mode)) ip->d.i_block[0] = (DEV_MAJOR(rdev) << 8) | DEV_MINOR(rdev);
  iupdate(ip);

  int r = 0;
  if (dir) {
    u32 blk;
    bool fresh;
    r = bmap(ip, 0, true, &blk, &fresh);
    if (r == 0) {
      struct buf *b = bget_zero(blk);
      struct ext2_dirent *dot = (struct ext2_dirent *)b->data;
      dot->inode = inum;
      dot->rec_len = 12;
      dot->name_len = 1;
      dot->file_type = fs.filetype ? EXT2_FT_DIR : 0;
      dot->name[0] = '.';
      struct ext2_dirent *dotdot = (struct ext2_dirent *)(b->data + 12);
      dotdot->inode = dp->inum;
      dotdot->rec_len = (u16)(fs.bsize - 12);
      dotdot->name_len = 2;
      dotdot->file_type = fs.filetype ? EXT2_FT_DIR : 0;
      dotdot->name[0] = dotdot->name[1] = '.';
      bwrite(b);
      brelse(b);
      ip->d.i_size = fs.bsize;
      iupdate(ip);
    }
  }
  if (r == 0) r = dirlink(dp, name, inum, mode);
  if (r < 0) {
    ip->d.i_links_count = 0;
    iupdate(ip);
    iunlockput(ip);   // frees the inode and any block it got
    return r;
  }
  if (dir) {
    dp->d.i_links_count++;   // the child's ".."
    iupdate(dp);
  }
  *out = ip;
  return 0;
}

// Fill dirent records from the directory position *off. Returns bytes
// stored, 0 at the end, or -EINVAL if not even one record fits.
i64 dir_getdents(struct inode *dp, u64 *off, u64 udst, u64 n) {
  struct proc *p = myproc();
  u64 out = 0;
  u8 rec[sizeof(struct dirent) + NAME_MAX + 8];
  while (*off < dp->d.i_size) {
    u32 lbn = (u32)(*off / fs.bsize);
    u32 want = (u32)(*off % fs.bsize);
    struct buf *b = dir_block(dp, lbn);
    if (!b) {
      *off = (u64)(lbn + 1) * fs.bsize;
      continue;
    }
    // Re-find an entry boundary at or after the saved offset: the entry
    // we stopped at may have been merged away since the last call.
    u32 pos = 0;
    while (pos < want) {
      struct ext2_dirent *de = (struct ext2_dirent *)(b->data + pos);
      if (!dirent_ok(de, pos)) { pos = fs.bsize; break; }
      pos += de->rec_len;
    }
    bool full = false;
    while (pos < fs.bsize) {
      struct ext2_dirent *de = (struct ext2_dirent *)(b->data + pos);
      if (!dirent_ok(de, pos)) { pos = fs.bsize; break; }
      if (de->inode) {
        u16 reclen = (u16)ROUNDUP(sizeof(struct dirent) + de->name_len + 1, 8);
        if (out + reclen > n) { full = true; break; }
        struct dirent *d = (struct dirent *)rec;
        memset(rec, 0, reclen);
        d->d_ino = de->inode;
        d->d_reclen = reclen;
        d->d_type = ftype_to_dt(de->file_type);
        d->d_namlen = de->name_len;
        memcpy(d->d_name, de->name, de->name_len);
        if (copyout(p, udst + out, rec, reclen) < 0) {
          brelse(b);
          return out ? (i64)out : -EFAULT;
        }
        out += reclen;
      }
      pos += de->rec_len;
    }
    brelse(b);
    *off = (u64)lbn * fs.bsize + pos;
    if (full) break;
  }
  if (out == 0 && *off < dp->d.i_size) return -EINVAL;
  return (i64)out;
}

// ---- path names ----

// Copy the next path element into name and return the rest of the path,
// or NULL when there are no more elements.
static const char *skipelem(const char *path, char *name, int *err) {
  while (*path == '/') path++;
  if (*path == '\0') return NULL;
  const char *s = path;
  while (*path && *path != '/') path++;
  size_t len = (size_t)(path - s);
  if (len > NAME_MAX) {
    *err = -ENAMETOOLONG;
    return NULL;
  }
  memcpy(name, s, len);
  name[len] = '\0';
  while (*path == '/') path++;
  return path;
}

static int namex(const char *path, bool parent, char *name, struct inode **out) {
  if (*path == '\0') return -ENOENT;
  struct inode *ip = *path == '/' ? iget(EXT2_ROOT_INO) : idup(myproc()->cwd);
  char elem[NAME_MAX + 1];
  int err = 0;
  const char *rest = path;
  while ((rest = skipelem(rest, elem, &err)) != NULL) {
    ilock(ip);
    if (!S_ISDIR(ip->d.i_mode)) {
      iunlockput(ip);
      return -ENOTDIR;
    }
    if (parent && *rest == '\0') {
      iunlock(ip);
      strlcpy(name, elem, NAME_MAX + 1);
      *out = ip;
      return 0;
    }
    struct inode *next = dirlookup(ip, elem, NULL);
    iunlockput(ip);
    if (!next) return -ENOENT;
    ip = next;
  }
  if (err) {
    iput(ip);
    return err;
  }
  if (parent) {   // the path names the root itself
    iput(ip);
    return -EBUSY;
  }
  *out = ip;
  return 0;
}

int namei(const char *path, struct inode **out) {
  return namex(path, false, NULL, out);
}

// The parent directory of path (unlocked) and the final element in name,
// which must hold NAME_MAX + 1 bytes.
int nameiparent(const char *path, char *name, struct inode **out) {
  return namex(path, true, name, out);
}

// Find the name under which dp (locked) lists inode inum.
static int dir_name_of(struct inode *dp, u32 inum, char *name) {
  for (u32 lbn = 0; lbn < dp->d.i_size / fs.bsize; lbn++) {
    struct buf *b = dir_block(dp, lbn);
    if (!b) continue;
    for (u32 pos = 0; pos < fs.bsize;) {
      struct ext2_dirent *de = (struct ext2_dirent *)(b->data + pos);
      if (!dirent_ok(de, pos)) break;
      if (de->inode == inum && !is_dot(de)) {
        memcpy(name, de->name, de->name_len);
        name[de->name_len] = '\0';
        brelse(b);
        return 0;
      }
      pos += de->rec_len;
    }
    brelse(b);
  }
  return -ENOENT;
}

// Build the absolute path of cwd by walking ".." up to the root.
int fs_getcwd(struct inode *cwd, char *buf, int size) {
  char *path = kalloc();
  if (!path) return -ENOMEM;
  char *p = path + PGSIZE;
  *--p = '\0';
  char name[NAME_MAX + 1];
  int r = 0;
  struct inode *ip = idup(cwd);
  while (ip->inum != EXT2_ROOT_INO) {
    ilock(ip);
    struct inode *parent = dirlookup(ip, "..", NULL);
    iunlock(ip);
    if (!parent) { r = -ENOENT; break; }
    ilock(parent);
    r = dir_name_of(parent, ip->inum, name);
    iunlock(parent);
    iput(ip);
    ip = parent;
    if (r < 0) break;
    size_t len = strlen(name);
    if ((u64)(p - path) < len + 1) { r = -ENAMETOOLONG; break; }
    p -= len;
    memcpy(p, name, len);
    *--p = '/';
  }
  iput(ip);
  if (r == 0) {
    if (*p == '\0') *--p = '/';
    size_t len = strlen(p);
    if ((int)len + 1 > size) r = -ERANGE;
    else memcpy(buf, p, len + 1);
  }
  kfree(path);
  return r < 0 ? r : 0;
}

void fs_statfs(u64 *blocks, u64 *bfree_out, u32 *bsize, u64 *inodes, u64 *ifree_out) {
  *blocks = fs.nblocks;
  *bfree_out = fs.sb.s_free_blocks_count;
  *bsize = fs.bsize;
  *inodes = fs.sb.s_inodes_count;
  *ifree_out = fs.sb.s_free_inodes_count;
}
