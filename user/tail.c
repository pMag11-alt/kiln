// tail [-n N] [file]: the last N lines (kept in a ring of line copies).
#include "user.h"

int main(int argc, char *argv[]) {
  int n = 10;
  int i = 1;
  if (i + 1 < argc && strcmp(argv[i], "-n") == 0) {
    n = atoi(argv[i + 1]);
    i += 2;
  }
  if (n <= 0) return 0;
  int fd = 0;
  if (i < argc && (fd = open(argv[i], O_RDONLY)) < 0) die("tail: %s: %s", argv[i], strerror(errno));
  char **ring = calloc((size_t)n, sizeof(char *));
  struct reader r;
  reader_init(&r, fd);
  char line[2048];
  long count = 0;
  while (reader_getline(&r, line, sizeof(line)) >= 0) {
    free(ring[count % n]);
    ring[count % n] = strdup(line);
    count++;
  }
  long start = count > n ? count - n : 0;
  for (long k = start; k < count; k++) printf("%s\n", ring[k % n]);
  return 0;
}
