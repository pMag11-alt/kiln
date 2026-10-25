// clear: clear the terminal (ANSI).
#include "user.h"

int main(void) {
  printf("\033[H\033[2J");
  return 0;
}
