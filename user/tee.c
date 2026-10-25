// tee [-a] file: copy stdin to stdout and to a file.
#include "user.h"

int main(int argc, char *argv[]) {
  int flags = O_WRONLY | O_CREAT | O_TRUNC;
  int i = 1;
  if (i < argc && strcmp(argv[i], "-a") == 0) {
    flags = O_WRONLY | O_CREAT | O_APPEND;
    i++;
  }
  int out = -1;
  if (i < argc && (out = open(argv[i], flags, 0644)) < 0) die("tee: %s: %s", argv[i], strerror(errno));
  char buf[4096];
  ssize_t n;
  while ((n = read(0, buf, sizeof(buf))) > 0) {
    write(1, buf, (size_t)n);
    if (out >= 0) write(out, buf, (size_t)n);
  }
  return 0;
}
