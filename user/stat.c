// stat path...
#include "user.h"

int main(int argc, char *argv[]) {
  if (argc < 2) die("usage: stat path...");
  int rc = 0;
  for (int i = 1; i < argc; i++) {
    struct stat st;
    if (stat(argv[i], &st) < 0) {
      dprintf(2, "stat: %s: %s\n", argv[i], strerror(errno));
      rc = 1;
      continue;
    }
    const char *type = S_ISDIR(st.st_mode) ? "directory" : S_ISREG(st.st_mode) ? "regular file"
                     : S_ISCHR(st.st_mode) ? "character device" : "other";
    printf("  File: %s\n  Size: %lu\tBlocks: %u\t%s\n Inode: %u\tLinks: %u\tMode: %04o\n",
           argv[i], (unsigned long)st.st_size, st.st_blocks, type, st.st_ino, st.st_nlink,
           st.st_mode & 07777);
    if (S_ISCHR(st.st_mode)) printf("Device: %u,%u\n", DEV_MAJOR(st.st_rdev), DEV_MINOR(st.st_rdev));
  }
  return rc;
}
