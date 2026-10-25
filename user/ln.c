// ln target linkname: make a hard link.
#include "user.h"

int main(int argc, char *argv[]) {
  if (argc != 3) die("usage: ln target linkname");
  if (link(argv[1], argv[2]) < 0) die("ln: %s -> %s: %s", argv[2], argv[1], strerror(errno));
  return 0;
}
