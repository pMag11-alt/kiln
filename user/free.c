// free: physical memory and kernel counters.
#include "user.h"

int main(void) {
  struct sysinfo si;
  if (sysinfo(&si) < 0) die("free: %s", strerror(errno));
  unsigned long total = si.total_pages * 4, freek = si.free_pages * 4;
  printf("memory:  %8lu KiB total %8lu KiB used %8lu KiB free\n", total, total - freek, freek);
  printf("faults:  %8lu copy-on-write %8lu demand-zero\n", (unsigned long)si.cow_faults,
         (unsigned long)si.lazy_faults);
  printf("disk:    %8lu reads   %8lu writes   cache %lu hits / %lu misses\n",
         (unsigned long)si.disk_reads, (unsigned long)si.disk_writes,
         (unsigned long)si.bcache_hits, (unsigned long)si.bcache_misses);
  printf("kernel:  %8lu syscalls %8lu context switches\n", (unsigned long)si.syscalls,
         (unsigned long)si.ctx_switches);
  return 0;
}
