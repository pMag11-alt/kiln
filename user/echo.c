// echo [-n] args...
#include "user.h"

int main(int argc, char *argv[]) {
  int i = 1;
  bool newline = true;
  if (argc > 1 && strcmp(argv[1], "-n") == 0) {
    newline = false;
    i++;
  }
  for (; i < argc; i++) printf("%s%s", argv[i], i + 1 < argc ? " " : "");
  if (newline) printf("\n");
  return 0;
}
