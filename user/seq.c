// seq [first] last
#include "user.h"

int main(int argc, char *argv[]) {
  if (argc < 2 || argc > 3) die("usage: seq [first] last");
  long first = argc == 3 ? strtol(argv[1], NULL, 10) : 1;
  long last = strtol(argv[argc - 1], NULL, 10);
  for (long i = first; i <= last; i++) printf("%ld\n", i);
  return 0;
}
