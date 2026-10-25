// sort [-r] [-n] [file]
#include "user.h"

static bool reverse, numeric;

static int cmp(const void *a, const void *b) {
  const char *x = *(char *const *)a, *y = *(char *const *)b;
  int r;
  if (numeric) {
    long u = strtol(x, NULL, 10), v = strtol(y, NULL, 10);
    r = u < v ? -1 : u > v ? 1 : strcmp(x, y);
  } else {
    r = strcmp(x, y);
  }
  return reverse ? -r : r;
}

int main(int argc, char *argv[]) {
  int i = 1;
  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
    for (char *f = argv[i] + 1; *f; f++) {
      if (*f == 'r') reverse = true;
      else if (*f == 'n') numeric = true;
      else die("usage: sort [-rn] [file]");
    }
  }
  int fd = 0;
  if (i < argc && (fd = open(argv[i], O_RDONLY)) < 0) die("sort: %s: %s", argv[i], strerror(errno));
  size_t cap = 256, n = 0;
  char **lines = malloc(cap * sizeof(char *));
  struct reader r;
  reader_init(&r, fd);
  char line[2048];
  while (reader_getline(&r, line, sizeof(line)) >= 0) {
    if (n == cap) {
      cap *= 2;
      lines = realloc(lines, cap * sizeof(char *));
      if (!lines) die("sort: out of memory");
    }
    lines[n++] = strdup(line);
  }
  qsort(lines, n, sizeof(char *), cmp);
  for (size_t k = 0; k < n; k++) printf("%s\n", lines[k]);
  return 0;
}
