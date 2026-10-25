// crc32 [file...]: zlib-compatible CRC-32 and size of each file.
#include "user.h"

static int sum(int fd, const char *name) {
  static char buf[16384];
  uint32_t crc = 0;
  unsigned long size = 0;
  ssize_t n;
  while ((n = read(fd, buf, sizeof(buf))) > 0) {
    crc = crc32_update(crc, buf, (size_t)n);
    size += (unsigned long)n;
  }
  if (n < 0) {
    dprintf(2, "crc32: %s: %s\n", name, strerror(errno));
    return 1;
  }
  printf("%08x %lu %s\n", crc, size, name);
  return 0;
}

int main(int argc, char *argv[]) {
  if (argc < 2) return sum(0, "-");
  int rc = 0;
  for (int i = 1; i < argc; i++) {
    int fd = open(argv[i], O_RDONLY);
    if (fd < 0) {
      dprintf(2, "crc32: %s: %s\n", argv[i], strerror(errno));
      rc = 1;
      continue;
    }
    rc |= sum(fd, argv[i]);
    close(fd);
  }
  return rc;
}
