// touch file...: create empty files (existing files are left alone).
#include "user.h"

int main(int argc, char *argv[]) {
  if (argc < 2) die("usage: touch file...");
  int rc = 0;
  for (int i = 1; i < argc; i++) {
    int fd = open(argv[i], O_WRONLY | O_CREAT, 0644);
    if (fd < 0) {
      dprintf(2, "touch: %s: %s\n", argv[i], strerror(errno));
      rc = 1;
      continue;
    }
    close(fd);
  }
  return rc;
}
