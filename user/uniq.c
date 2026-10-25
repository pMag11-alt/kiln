// uniq [-c]: collapse adjacent duplicate lines of stdin.
#include "user.h"

int main(int argc, char *argv[]) {
  bool counts = argc > 1 && strcmp(argv[1], "-c") == 0;
  struct reader r;
  reader_init(&r, 0);
  static char prev[2048], line[2048];
  long run = 0;
  while (reader_getline(&r, line, sizeof(line)) >= 0) {
    if (run && strcmp(line, prev) == 0) {
      run++;
      continue;
    }
    if (run) {
      if (counts) printf("%7ld %s\n", run, prev);
      else printf("%s\n", prev);
    }
    strcpy(prev, line);
    run = 1;
  }
  if (run) {
    if (counts) printf("%7ld %s\n", run, prev);
    else printf("%s\n", prev);
  }
  return 0;
}
