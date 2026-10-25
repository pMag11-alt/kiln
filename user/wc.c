// wc [-lwc] [file...]: lines, words and bytes.
#include "user.h"

static bool want_l, want_w, want_c;
static unsigned long tl, tw, tc;

static void report(unsigned long l, unsigned long w, unsigned long c, const char *name) {
  if (want_l) printf("%7lu", l);
  if (want_w) printf("%s%7lu", want_l ? " " : "", w);
  if (want_c) printf("%s%7lu", want_l || want_w ? " " : "", c);
  printf("%s%s\n", name[0] ? " " : "", name);
}

static int count(int fd, const char *name) {
  char buf[4096];
  unsigned long l = 0, w = 0, c = 0;
  bool inword = false;
  ssize_t n;
  while ((n = read(fd, buf, sizeof(buf))) > 0) {
    for (ssize_t i = 0; i < n; i++) {
      c++;
      if (buf[i] == '\n') l++;
      if (isspace(buf[i])) inword = false;
      else if (!inword) {
        inword = true;
        w++;
      }
    }
  }
  if (n < 0) {
    dprintf(2, "wc: %s: %s\n", name, strerror(errno));
    return 1;
  }
  report(l, w, c, name);
  tl += l;
  tw += w;
  tc += c;
  return 0;
}

int main(int argc, char *argv[]) {
  int i = 1;
  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
    for (char *f = argv[i] + 1; *f; f++) {
      if (*f == 'l') want_l = true;
      else if (*f == 'w') want_w = true;
      else if (*f == 'c') want_c = true;
      else die("usage: wc [-lwc] [file...]");
    }
  }
  if (!want_l && !want_w && !want_c) want_l = want_w = want_c = true;
  if (i == argc) return count(0, "");
  int rc = 0, files = argc - i;
  for (; i < argc; i++) {
    int fd = open(argv[i], O_RDONLY);
    if (fd < 0) {
      dprintf(2, "wc: %s: %s\n", argv[i], strerror(errno));
      rc = 1;
      continue;
    }
    rc |= count(fd, argv[i]);
    close(fd);
  }
  if (files > 1) report(tl, tw, tc, "total");
  return rc;
}
