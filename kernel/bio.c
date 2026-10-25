// Buffer cache: an LRU of file-system blocks, each guarded by a sleeplock.
// It is write-through: bwrite() reaches the disk before returning, so a
// power-off at any point loses at most the operation in flight.
#include "defs.h"

static struct {
  struct spinlock lock;
  struct buf buf[NBUF];
  struct buf head;   // head.next is most recently used
  u32 bsize;
} bcache;

void binit(void) {
  initlock(&bcache.lock, "bcache");
  bcache.bsize = 1024;
  bcache.head.prev = bcache.head.next = &bcache.head;
  for (struct buf *b = bcache.buf; b < bcache.buf + NBUF; b++) {
    initsleeplock(&b->lock, "buffer");
    b->next = bcache.head.next;
    b->prev = &bcache.head;
    bcache.head.next->prev = b;
    bcache.head.next = b;
  }
}

// Change the block size (once, at mount). Cached contents are discarded.
void bset_blocksize(u32 bsize) {
  if (bsize > BSIZE_MAX || bsize < 1024 || (bsize & (bsize - 1))) panic("bset_blocksize: %u", bsize);
  acquire(&bcache.lock);
  for (struct buf *b = bcache.buf; b < bcache.buf + NBUF; b++) {
    if (b->refcnt) panic("bset_blocksize: buffer in use");
    b->valid = false;
  }
  bcache.bsize = bsize;
  release(&bcache.lock);
}

u32 bblocksize(void) {
  return bcache.bsize;
}

// Find the buffer for blockno or recycle the least recently used idle one.
// Returns it locked.
static struct buf *bget(u32 blockno) {
  acquire(&bcache.lock);
  for (struct buf *b = bcache.head.next; b != &bcache.head; b = b->next) {
    if (b->blockno == blockno && (b->valid || b->refcnt)) {
      b->refcnt++;
      release(&bcache.lock);
      acquiresleep(&b->lock);
      return b;
    }
  }
  for (struct buf *b = bcache.head.prev; b != &bcache.head; b = b->prev) {
    if (b->refcnt == 0) {
      b->blockno = blockno;
      b->valid = false;
      b->refcnt = 1;
      release(&bcache.lock);
      acquiresleep(&b->lock);
      return b;
    }
  }
  panic("bget: no free buffers");
}

struct buf *bread(u32 blockno) {
  struct buf *b = bget(blockno);
  if (!b->valid) {
    STAT_INC(bcache_misses);
    virtio_blk_rw(b, bcache.bsize, false);
    b->valid = true;
  } else {
    STAT_INC(bcache_hits);
  }
  return b;
}

// A buffer for a block whose old contents are irrelevant (freshly
// allocated): zero-filled, no disk read.
struct buf *bget_zero(u32 blockno) {
  struct buf *b = bget(blockno);
  memset(b->data, 0, bcache.bsize);
  b->valid = true;
  return b;
}

void bwrite(struct buf *b) {
  if (!holdingsleep(&b->lock)) panic("bwrite: buffer not locked");
  virtio_blk_rw(b, bcache.bsize, true);
}

void brelse(struct buf *b) {
  if (!holdingsleep(&b->lock)) panic("brelse: buffer not locked");
  releasesleep(&b->lock);
  acquire(&bcache.lock);
  if (--b->refcnt == 0) {
    // Move to the front of the LRU list.
    b->next->prev = b->prev;
    b->prev->next = b->next;
    b->next = bcache.head.next;
    b->prev = &bcache.head;
    bcache.head.next->prev = b;
    bcache.head.next = b;
  }
  release(&bcache.lock);
}
