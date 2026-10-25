// virtio block device over the MMIO transport (virtio 1.x, "modern").
//
// Each request is a chain of three descriptors: a header the device reads,
// the data buffer, and a status byte the device writes. Callers sleep until
// the completion interrupt marks their buffer done.
#include "defs.h"

// MMIO register offsets
#define VIRTIO_MAGIC          0x000
#define VIRTIO_VERSION        0x004
#define VIRTIO_DEVICE_ID      0x008
#define VIRTIO_DEVICE_FEATURES     0x010
#define VIRTIO_DEVICE_FEATURES_SEL 0x014
#define VIRTIO_DRIVER_FEATURES     0x020
#define VIRTIO_DRIVER_FEATURES_SEL 0x024
#define VIRTIO_QUEUE_SEL      0x030
#define VIRTIO_QUEUE_NUM_MAX  0x034
#define VIRTIO_QUEUE_NUM      0x038
#define VIRTIO_QUEUE_READY    0x044
#define VIRTIO_QUEUE_NOTIFY   0x050
#define VIRTIO_INTR_STATUS    0x060
#define VIRTIO_INTR_ACK       0x064
#define VIRTIO_STATUS         0x070
#define VIRTIO_QUEUE_DESC_LO  0x080
#define VIRTIO_QUEUE_DESC_HI  0x084
#define VIRTIO_QUEUE_AVAIL_LO 0x090
#define VIRTIO_QUEUE_AVAIL_HI 0x094
#define VIRTIO_QUEUE_USED_LO  0x0a0
#define VIRTIO_QUEUE_USED_HI  0x0a4
#define VIRTIO_CONFIG         0x100

#define STATUS_ACKNOWLEDGE 1
#define STATUS_DRIVER      2
#define STATUS_DRIVER_OK   4
#define STATUS_FEATURES_OK 8
#define STATUS_FAILED      128

#define VIRTIO_F_VERSION_1 32   // bit in the second feature word

#define VIRTQ_DESC_F_NEXT  1
#define VIRTQ_DESC_F_WRITE 2

#define VIRTIO_BLK_T_IN  0
#define VIRTIO_BLK_T_OUT 1

#define QSIZE 16

struct virtq_desc {
  u64 addr;
  u32 len;
  u16 flags;
  u16 next;
};

struct virtq_avail {
  u16 flags;
  u16 idx;
  u16 ring[QSIZE];
  u16 used_event;
};

struct virtq_used_elem {
  u32 id;
  u32 len;
};

struct virtq_used {
  u16 flags;
  u16 idx;
  struct virtq_used_elem ring[QSIZE];
  u16 avail_event;
};

struct blk_req {
  u32 type;
  u32 reserved;
  u64 sector;
};

static struct {
  struct spinlock lock;
  u64 base;
  u32 irq;
  bool present;
  struct virtq_desc *desc;
  struct virtq_avail *avail;
  struct virtq_used *used;
  bool free[QSIZE];
  u16 used_idx;           // how far we have consumed the used ring
  struct {
    struct buf *b;
    volatile u8 status;
    volatile bool done;
  } info[QSIZE];
  struct blk_req req[QSIZE];
  u64 capacity;           // in 512-byte sectors
} disk;

static inline volatile u32 *reg(u32 off) {
  return (volatile u32 *)((u8 *)P2V(disk.base) + off);
}

void virtio_blk_init(void) {
  initlock(&disk.lock, "virtio_blk");
  for (int i = 0; i < plat.nvirtio; i++) {
    disk.base = plat.virtio_base[i];
    if (*reg(VIRTIO_MAGIC) == 0x74726976 && *reg(VIRTIO_DEVICE_ID) == 2) {
      disk.irq = plat.virtio_irq[i];
      disk.present = true;
      break;
    }
  }
  if (!disk.present) panic("virtio_blk: no block device (attach one with -drive)");
  if (*reg(VIRTIO_VERSION) != 2)
    panic("virtio_blk: legacy device; run QEMU with -global virtio-mmio.force-legacy=false");

  u32 status = 0;
  *reg(VIRTIO_STATUS) = status;   // reset
  status |= STATUS_ACKNOWLEDGE;
  *reg(VIRTIO_STATUS) = status;
  status |= STATUS_DRIVER;
  *reg(VIRTIO_STATUS) = status;

  // We need nothing beyond the base protocol: accept only VERSION_1.
  *reg(VIRTIO_DEVICE_FEATURES_SEL) = 1;
  if (!(*reg(VIRTIO_DEVICE_FEATURES) & (1u << (VIRTIO_F_VERSION_1 - 32))))
    panic("virtio_blk: device lacks VIRTIO_F_VERSION_1");
  *reg(VIRTIO_DRIVER_FEATURES_SEL) = 0;
  *reg(VIRTIO_DRIVER_FEATURES) = 0;
  *reg(VIRTIO_DRIVER_FEATURES_SEL) = 1;
  *reg(VIRTIO_DRIVER_FEATURES) = 1u << (VIRTIO_F_VERSION_1 - 32);
  status |= STATUS_FEATURES_OK;
  *reg(VIRTIO_STATUS) = status;
  if (!(*reg(VIRTIO_STATUS) & STATUS_FEATURES_OK)) panic("virtio_blk: features rejected");

  *reg(VIRTIO_QUEUE_SEL) = 0;
  if (*reg(VIRTIO_QUEUE_READY)) panic("virtio_blk: queue already live");
  u32 max = *reg(VIRTIO_QUEUE_NUM_MAX);
  if (max < QSIZE) panic("virtio_blk: queue too small (%u)", max);
  disk.desc = kzalloc();
  disk.avail = kzalloc();
  disk.used = kzalloc();
  if (!disk.desc || !disk.avail || !disk.used) panic("virtio_blk: out of memory");
  *reg(VIRTIO_QUEUE_NUM) = QSIZE;
  u64 d = V2P(disk.desc), a = V2P(disk.avail), u = V2P(disk.used);
  *reg(VIRTIO_QUEUE_DESC_LO) = (u32)d;
  *reg(VIRTIO_QUEUE_DESC_HI) = (u32)(d >> 32);
  *reg(VIRTIO_QUEUE_AVAIL_LO) = (u32)a;
  *reg(VIRTIO_QUEUE_AVAIL_HI) = (u32)(a >> 32);
  *reg(VIRTIO_QUEUE_USED_LO) = (u32)u;
  *reg(VIRTIO_QUEUE_USED_HI) = (u32)(u >> 32);
  *reg(VIRTIO_QUEUE_READY) = 1;

  for (int i = 0; i < QSIZE; i++) disk.free[i] = true;

  status |= STATUS_DRIVER_OK;
  *reg(VIRTIO_STATUS) = status;

  disk.capacity = (u64)reg(VIRTIO_CONFIG)[0] | ((u64)reg(VIRTIO_CONFIG)[1] << 32);
  kprintf("virtio-blk: %lu MiB at %p, irq %u\n", disk.capacity / 2048,
          (void *)disk.base, disk.irq);
}

bool virtio_blk_owns_irq(int irq) {
  return disk.present && (u32)irq == disk.irq;
}

static int alloc_desc(void) {
  for (int i = 0; i < QSIZE; i++) {
    if (disk.free[i]) {
      disk.free[i] = false;
      return i;
    }
  }
  return -1;
}

static void free_desc(int i) {
  disk.desc[i] = (struct virtq_desc){0};
  disk.free[i] = true;
  wakeup(&disk.free[0]);
}

static void free_chain(int i) {
  for (;;) {
    u16 flags = disk.desc[i].flags;
    u16 next = disk.desc[i].next;
    free_desc(i);
    if (!(flags & VIRTQ_DESC_F_NEXT)) break;
    i = next;
  }
}

static bool alloc3(int idx[3]) {
  for (int i = 0; i < 3; i++) {
    idx[i] = alloc_desc();
    if (idx[i] < 0) {
      for (int j = 0; j < i; j++) free_desc(idx[j]);
      return false;
    }
  }
  return true;
}

void virtio_blk_rw(struct buf *b, u32 bsize, bool write) {
  u64 sector = (u64)b->blockno * (bsize / 512);
  if (sector + bsize / 512 > disk.capacity) panic("virtio_blk: block %u past end of disk", b->blockno);

  acquire(&disk.lock);
  int idx[3];
  while (!alloc3(idx)) sleep(&disk.free[0], &disk.lock);

  struct blk_req *req = &disk.req[idx[0]];
  req->type = write ? VIRTIO_BLK_T_OUT : VIRTIO_BLK_T_IN;
  req->reserved = 0;
  req->sector = sector;

  disk.desc[idx[0]] = (struct virtq_desc){V2P(req), sizeof(*req), VIRTQ_DESC_F_NEXT, (u16)idx[1]};
  disk.desc[idx[1]] = (struct virtq_desc){V2P(b->data), bsize,
                                          (u16)(VIRTQ_DESC_F_NEXT | (write ? 0 : VIRTQ_DESC_F_WRITE)),
                                          (u16)idx[2]};
  disk.info[idx[0]].status = 0xff;
  disk.info[idx[0]].done = false;
  disk.info[idx[0]].b = b;
  disk.desc[idx[2]] = (struct virtq_desc){V2P(&disk.info[idx[0]].status), 1, VIRTQ_DESC_F_WRITE, 0};

  b->disk = true;
  disk.avail->ring[disk.avail->idx % QSIZE] = (u16)idx[0];
  __sync_synchronize();   // descriptors and ring entry before the index
  disk.avail->idx++;
  __sync_synchronize();
  *reg(VIRTIO_QUEUE_NOTIFY) = 0;

  while (!disk.info[idx[0]].done) sleep(b, &disk.lock);
  if (disk.info[idx[0]].status != 0)
    panic("virtio_blk: I/O error %d on block %u", disk.info[idx[0]].status, b->blockno);
  disk.info[idx[0]].b = NULL;
  free_chain(idx[0]);
  release(&disk.lock);

  if (write) STAT_INC(disk_writes);
  else STAT_INC(disk_reads);
}

void virtio_blk_intr(void) {
  acquire(&disk.lock);
  // Acknowledge first so a completion that races with us raises a new IRQ.
  *reg(VIRTIO_INTR_ACK) = *reg(VIRTIO_INTR_STATUS) & 0x3;
  __sync_synchronize();
  while (disk.used_idx != disk.used->idx) {
    __sync_synchronize();
    int id = (int)disk.used->ring[disk.used_idx % QSIZE].id;
    struct buf *b = disk.info[id].b;
    if (!b) panic("virtio_blk: completion for idle descriptor %d", id);
    b->disk = false;
    disk.info[id].done = true;
    wakeup(b);
    disk.used_idx++;
  }
  release(&disk.lock);
}
