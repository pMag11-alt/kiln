// rmdir dir...
#include "user.h"

int main(int argc, char *argv[]) {
  if (argc < 2) die("usage: rmdir dir...");
  int rc = 0;
  for (int i = 1; i < argc; i++) {
    if (rmdir(argv[i]) < 0) {
      dprintf(2, "rmdir: %s: %s\n", argv[i], strerror(errno));
      rc = 1;
    }
  }
  return rc;
}
