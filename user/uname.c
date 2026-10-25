// uname [-a]
#include "user.h"

int main(int argc, char *argv[]) {
  if (argc > 1 && strcmp(argv[1], "-a") == 0) printf("kiln 0.1 riscv64 (Sv39, SMP, ext2)\n");
  else printf("kiln\n");
  return 0;
}
