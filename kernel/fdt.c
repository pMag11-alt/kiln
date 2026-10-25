// A minimal flattened-device-tree reader. It walks the structure block once
// and records the handful of devices the kernel drives, so nothing about the
// board's memory map is hard-coded.
#include "defs.h"

struct platform plat = {
  // Sensible QEMU virt defaults so early printing works before parsing.
  .uart_base = 0x10000000,
  .uart_irq = 10,
  .timebase = 10000000,
};

#define FDT_MAGIC      0xd00dfeed
#define FDT_BEGIN_NODE 1
#define FDT_END_NODE   2
#define FDT_PROP       3
#define FDT_NOP        4
#define FDT_END        9

struct fdt_header {
  u32 magic, totalsize, off_dt_struct, off_dt_strings, off_mem_rsvmap;
  u32 version, last_comp_version, boot_cpuid_phys, size_dt_strings, size_dt_struct;
};

static u32 be32(const void *p) {
  const u8 *b = p;
  return (u32)b[0] << 24 | (u32)b[1] << 16 | (u32)b[2] << 8 | b[3];
}

static u64 be_cells(const u8 *p, int cells) {
  u64 v = 0;
  for (int i = 0; i < cells; i++) v = (v << 32) | be32(p + 4 * i);
  return v;
}

// Properties of one node that we care about.
struct node {
  const char *name;
  const char *compat;  int compat_len;
  const u8 *reg;       int reg_len;
  const u8 *irq;       int irq_len;
  const char *devtype;
  const char *status;
  int addr_cells, size_cells;   // for this node's children
};

static bool has_compat(const struct node *n, const char *want) {
  // "compatible" is a list of NUL-separated strings.
  for (int i = 0; i < n->compat_len;) {
    const char *s = n->compat + i;
    if (strcmp(s, want) == 0) return true;
    i += strlen(s) + 1;
  }
  return false;
}

static void record(const struct node *n, const struct node *parent) {
  int ac = parent ? parent->addr_cells : 2;
  int sc = parent ? parent->size_cells : 1;
  u64 base = 0, size = 0;
  if (n->reg && n->reg_len >= 4 * (ac + sc)) {
    base = be_cells(n->reg, ac);
    size = be_cells(n->reg + 4 * ac, sc);
  }
  u32 irq = n->irq && n->irq_len >= 4 ? be32(n->irq) : 0;
  bool okay = !n->status || strcmp(n->status, "okay") == 0 || strcmp(n->status, "ok") == 0;

  if (n->devtype && strcmp(n->devtype, "memory") == 0 && !plat.ram_size) {
    plat.ram_base = base;
    plat.ram_size = size;
  } else if (n->devtype && strcmp(n->devtype, "cpu") == 0 && okay) {
    if (plat.nharts < NCPU && base < NCPU) plat.harts[plat.nharts++] = base;
  } else if (has_compat(n, "ns16550a")) {
    plat.uart_base = base;
    plat.uart_irq = irq;
  } else if (has_compat(n, "riscv,plic0") || has_compat(n, "sifive,plic-1.0.0")) {
    plat.plic_base = base;
    plat.plic_size = size;
  } else if (has_compat(n, "google,goldfish-rtc")) {
    plat.rtc_base = base;
  } else if (has_compat(n, "virtio,mmio")) {
    if (plat.nvirtio < MAX_VIRTIO) {
      plat.virtio_base[plat.nvirtio] = base;
      plat.virtio_irq[plat.nvirtio] = irq;
      plat.nvirtio++;
    }
  } else if (parent && parent->name && strcmp(parent->name, "reserved-memory") == 0) {
    if (plat.nresv < MAX_RESERVED && size) {
      plat.resv_base[plat.nresv] = base;
      plat.resv_size[plat.nresv] = size;
      plat.nresv++;
    }
  }
}

void fdt_parse(const void *blob) {
  const struct fdt_header *h = blob;
  if (be32(&h->magic) != FDT_MAGIC) panic("fdt: bad magic %x", be32(&h->magic));
  const u8 *base = blob;
  const u8 *p = base + be32(&h->off_dt_struct);
  const u8 *pend = p + be32(&h->size_dt_struct);
  const char *strings = (const char *)base + be32(&h->off_dt_strings);

  // Memory reservation block: (address, size) pairs ending in a zero pair.
  for (const u8 *r = base + be32(&h->off_mem_rsvmap);; r += 16) {
    u64 a = be_cells(r, 2), s = be_cells(r + 8, 2);
    if (!a && !s) break;
    if (plat.nresv < MAX_RESERVED) {
      plat.resv_base[plat.nresv] = a;
      plat.resv_size[plat.nresv] = s;
      plat.nresv++;
    }
  }

  struct node stack[12];
  int depth = -1;
  while (p < pend) {
    u32 tok = be32(p);
    p += 4;
    if (tok == FDT_BEGIN_NODE) {
      const char *name = (const char *)p;
      p += ROUNDUP(strlen(name) + 1, 4);
      if (++depth >= (int)ARRAY_SIZE(stack)) panic("fdt: tree too deep");
      struct node *n = &stack[depth];
      memset(n, 0, sizeof(*n));
      n->name = name;
      n->addr_cells = 2;
      n->size_cells = 1;
    } else if (tok == FDT_END_NODE) {
      if (depth < 0) panic("fdt: unbalanced tree");
      record(&stack[depth], depth > 0 ? &stack[depth - 1] : NULL);
      depth--;
    } else if (tok == FDT_PROP) {
      u32 len = be32(p);
      const char *pname = strings + be32(p + 4);
      const u8 *val = p + 8;
      p += 8 + ROUNDUP(len, 4);
      if (depth < 0) continue;
      struct node *n = &stack[depth];
      if (!strcmp(pname, "compatible")) { n->compat = (const char *)val; n->compat_len = len; }
      else if (!strcmp(pname, "reg")) { n->reg = val; n->reg_len = len; }
      else if (!strcmp(pname, "interrupts")) { n->irq = val; n->irq_len = len; }
      else if (!strcmp(pname, "device_type")) n->devtype = (const char *)val;
      else if (!strcmp(pname, "status")) n->status = (const char *)val;
      else if (!strcmp(pname, "#address-cells")) n->addr_cells = be32(val);
      else if (!strcmp(pname, "#size-cells")) n->size_cells = be32(val);
      else if (!strcmp(pname, "timebase-frequency")) plat.timebase = be32(val);
      else if (!strcmp(pname, "bootargs")) strlcpy(plat.bootargs, (const char *)val, sizeof(plat.bootargs));
    } else if (tok == FDT_NOP) {
      continue;
    } else if (tok == FDT_END) {
      break;
    } else {
      panic("fdt: bad token %u", tok);
    }
  }

  if (!plat.ram_size) panic("fdt: no memory node");
  if (!plat.plic_base) panic("fdt: no PLIC");
}
