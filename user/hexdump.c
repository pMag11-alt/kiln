// hexdump [file]: canonical hex+ASCII dump (like hexdump -C).
#include "user.h"

int main(int argc, char *argv[]) {
  int fd = 0;
  if (argc > 1 && (fd = open(argv[1], O_RDONLY)) < 0) die("hexdump: %s: %s", argv[1], strerror(errno));
  unsigned char buf[16];
  unsigned long off = 0;
  for (;;) {
    ssize_t n = 0, got;
    while (n < 16 && (got = read(fd, buf + n, (size_t)(16 - n))) > 0) n += got;
    if (n <= 0) break;
    printf("%08lx  ", off);
    for (int i = 0; i < 16; i++) {
      if (i < n) printf("%02x ", buf[i]);
      else printf("   ");
      if (i == 7) printf(" ");
    }
    printf(" |");
    for (int i = 0; i < n; i++) putchar(isprint(buf[i]) ? buf[i] : '.');
    printf("|\n");
    off += (unsigned long)n;
    if (n < 16) break;
  }
  printf("%08lx\n", off);
  return 0;
}
