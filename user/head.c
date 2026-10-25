// head [-n N] [file]
#include "user.h"

int main(int argc, char *argv[]) {
  long n = 10;
  int i = 1;
  if (i + 1 < argc && strcmp(argv[i], "-n") == 0) {
    n = atoi(argv[i + 1]);
    i += 2;
  }
  int fd = 0;
  if (i < argc && (fd = open(argv[i], O_RDONLY)) < 0) die("head: %s: %s", argv[i], strerror(errno));
  char buf[4096];
  ssize_t got;
  while (n > 0 && (got = read(fd, buf, sizeof(buf))) > 0) {
    ssize_t k = 0;
    while (k < got && n > 0)
      if (buf[k++] == '\n') n--;
    write(1, buf, (size_t)k);
  }
  return 0;
}
