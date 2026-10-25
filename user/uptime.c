// uptime: time since boot, process count and harts.
#include "user.h"

int main(void) {
  struct sysinfo si;
  if (sysinfo(&si) < 0) die("uptime: %s", strerror(errno));
  unsigned long s = si.uptime_ms / 1000;
  printf("up %lu:%02lu:%02lu.%03lu, %u processes, %u harts\n", s / 3600, (s / 60) % 60, s % 60,
         (unsigned long)(si.uptime_ms % 1000), si.nproc, si.ncpu);
  return 0;
}
