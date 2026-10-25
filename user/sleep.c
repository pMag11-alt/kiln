// sleep seconds  (fractions like 0.5 are allowed)
#include "user.h"

int main(int argc, char *argv[]) {
  if (argc != 2) die("usage: sleep seconds");
  char *end;
  unsigned long ms = strtoul(argv[1], &end, 10) * 1000;
  if (*end == '.') {
    unsigned long scale = 100;
    for (char *p = end + 1; isdigit(*p) && scale; p++, scale /= 10) ms += (unsigned long)(*p - '0') * scale;
  }
  return msleep(ms) < 0 ? 1 : 0;
}
