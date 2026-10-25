// Kernel entry points for the boot hart and the secondary harts.
#include "defs.h"

// One boot stack per hart id; each later becomes that hart's scheduler stack.
__attribute__((aligned(16))) char boot_stacks[NCPU][BOOT_STACK_SIZE];
struct kstats kstats;

extern char _secondary_start[];

__noreturn void kmain(u64 hartid, u64 fdt_pa);
__noreturn void kmain_secondary(u64 hartid, u64 id);

void kmain(u64 hartid, u64 fdt_pa) {
  struct cpu *c = &cpus[0];
  c->hartid = hartid;
  c->id = 0;
  w_tp((u64)c);

  fdt_parse(P2V(fdt_pa));
  console_init();
  long sbi = sbi_spec_version();
  kprintf("\nkiln: RISC-V kernel, %lu MiB RAM, %d harts, SBI v%ld.%ld, booting on hart %lu\n",
          plat.ram_size >> 20, plat.nharts, (sbi >> 24) & 0x7f, sbi & 0xffffff, hartid);

  kinit();
  kvminit();
  kvminithart();
  procinit();
  trapinithart();
  plic_init();
  plic_inithart();
  binit();
  fileinit();
  virtio_blk_init();
  userinit();
  kprintf("kiln: %lu pages free\n", kmem_free_pages());

  c->online = true;
  timer_inithart();

  int next = 1;
  for (int i = 0; i < plat.nharts && next < NCPU; i++) {
    u64 h = plat.harts[i];
    if (h == hartid) continue;
    cpus[next].hartid = h;
    cpus[next].id = next;
    __sync_synchronize();
    long err = sbi_hart_start(h, V2P(_secondary_start), (u64)next);
    if (err) kprintf("kiln: could not start hart %lu (sbi error %ld)\n", h, err);
    else next++;
  }

  scheduler();
}

void kmain_secondary(u64 hartid, u64 id) {
  struct cpu *c = &cpus[id];
  w_tp((u64)c);
  if (c->hartid != hartid) panic("hart %lu started with the wrong cpu slot", hartid);
  kvminithart();
  trapinithart();
  plic_inithart();
  c->online = true;
  timer_inithart();
  scheduler();
}
