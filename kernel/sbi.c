// Calls into the Supervisor Binary Interface (OpenSBI on QEMU).
#include "defs.h"

#define SBI_EXT_BASE 0x10
#define SBI_EXT_TIME 0x54494D45   // "TIME"
#define SBI_EXT_HSM  0x48534D     // "HSM"
#define SBI_EXT_SRST 0x53525354   // "SRST"

struct sbiret {
  long error;
  long value;
};

static struct sbiret sbi_call(u64 ext, u64 fid, u64 a0, u64 a1, u64 a2) {
  register u64 r_a0 asm("a0") = a0;
  register u64 r_a1 asm("a1") = a1;
  register u64 r_a2 asm("a2") = a2;
  register u64 r_a6 asm("a6") = fid;
  register u64 r_a7 asm("a7") = ext;
  asm volatile("ecall"
               : "+r"(r_a0), "+r"(r_a1)
               : "r"(r_a2), "r"(r_a6), "r"(r_a7)
               : "memory");
  return (struct sbiret){(long)r_a0, (long)r_a1};
}

long sbi_spec_version(void) {
  return sbi_call(SBI_EXT_BASE, 0, 0, 0, 0).value;
}

void sbi_set_timer(u64 stime) {
  sbi_call(SBI_EXT_TIME, 0, stime, 0, 0);
}

long sbi_hart_start(u64 hartid, u64 start_pa, u64 opaque) {
  return sbi_call(SBI_EXT_HSM, 0, hartid, start_pa, opaque).error;
}

void sbi_system_reset(u32 type, u32 reason) {
  sbi_call(SBI_EXT_SRST, 0, type, reason, 0);
  // SRST returned: firmware without the extension. Spin.
  for (;;) wfi();
}
