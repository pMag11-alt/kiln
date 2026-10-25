// df: free space on the root file system.
#include "user.h"

int main(void) {
  struct sysinfo si;
  if (sysinfo(&si) < 0) die("df: %s", strerror(errno));
  unsigned long kb = si.fs_block_size / 1024;
  unsigned long total = si.fs_blocks * kb, avail = si.fs_free_blocks * kb;
  printf("Filesystem  1K-blocks     Used Available Use%%  Inodes  IFree Mounted on\n");
  printf("/dev/vda    %9lu %8lu %9lu %3lu%% %7lu %6lu /\n", total, total - avail, avail,
         total ? (100 * (total - avail) + total - 1) / total : 0, (unsigned long)si.fs_inodes,
         (unsigned long)si.fs_free_inodes);
  return 0;
}
