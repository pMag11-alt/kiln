// grep [-i] [-v] [-c] [-n] pattern [file...]
//
// Patterns are basic regular expressions without groups or alternation:
//   c  literal     .  any char     *  zero or more of the previous item
//   ^  start       $  end          [abc] [^a-z]  character classes
//   \c escape
#include "user.h"

static bool icase, invert, countonly, numbers;

// Return the length of the item at p (a char, '.', escape or [class]).
static int item_len(const char *p) {
  if (*p == '\\' && p[1]) return 2;
  if (*p == '[') {
    const char *q = p + 1;
    if (*q == '^') q++;
    if (*q == ']') q++;
    while (*q && *q != ']') q++;
    return *q ? (int)(q - p + 1) : 1;
  }
  return 1;
}

static bool item_match(const char *p, int len, char c) {
  if (icase) c = (char)tolower(c);
  if (len == 2 && p[0] == '\\') return (icase ? tolower(p[1]) : p[1]) == c;
  if (p[0] == '.' && len == 1) return true;
  if (p[0] == '[' && len > 1) {
    const char *q = p + 1, *end = p + len - 1;
    bool neg = *q == '^';
    if (neg) q++;
    bool hit = false;
    for (bool first = true; q < end; first = false) {
      char lo = *q;
      if (!first && lo == ']') break;
      if (q + 2 < end && q[1] == '-') {
        char hi = q[2];
        if (icase) { lo = (char)tolower(lo); hi = (char)tolower(hi); }
        if (c >= lo && c <= hi) hit = true;
        q += 3;
      } else {
        if ((icase ? tolower(lo) : lo) == c) hit = true;
        q++;
      }
    }
    return hit != neg;
  }
  return (icase ? tolower(p[0]) : p[0]) == c;
}

static bool match_here(const char *re, const char *s);

static bool match_star(const char *item, int len, const char *re, const char *s) {
  do {
    if (match_here(re, s)) return true;
  } while (*s && item_match(item, len, *s++));
  return false;
}

static bool match_here(const char *re, const char *s) {
  if (!*re) return true;
  int len = item_len(re);
  if (re[len] == '*') return match_star(re, len, re + len + 1, s);
  if (re[0] == '$' && !re[1]) return !*s;
  if (*s && item_match(re, len, *s)) return match_here(re + len, s + 1);
  return false;
}

static bool matches(const char *re, const char *s) {
  if (re[0] == '^') return match_here(re + 1, s);
  do {
    if (match_here(re, s)) return true;
  } while (*s++);
  return false;
}

static int grep(const char *re, int fd, const char *name, bool show_name) {
  struct reader r;
  reader_init(&r, fd);
  char line[2048];
  unsigned long lineno = 0, hits = 0;
  while (reader_getline(&r, line, sizeof(line)) >= 0) {
    lineno++;
    if (matches(re, line) == invert) continue;
    hits++;
    if (countonly) continue;
    if (show_name) printf("%s:", name);
    if (numbers) printf("%lu:", lineno);
    printf("%s\n", line);
  }
  if (countonly) {
    if (show_name) printf("%s:", name);
    printf("%lu\n", hits);
  }
  return hits ? 0 : 1;
}

int main(int argc, char *argv[]) {
  int i = 1;
  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
    for (char *f = argv[i] + 1; *f; f++) {
      if (*f == 'i') icase = true;
      else if (*f == 'v') invert = true;
      else if (*f == 'c') countonly = true;
      else if (*f == 'n') numbers = true;
      else die("usage: grep [-ivcn] pattern [file...]");
    }
  }
  if (i >= argc) die("usage: grep [-ivcn] pattern [file...]");
  char *re = argv[i++];
  if (icase)
    for (char *p = re; *p; p++) *p = (char)tolower(*p);
  if (i == argc) return grep(re, 0, "stdin", false);
  int rc = 1;
  bool show = argc - i > 1;
  for (; i < argc; i++) {
    int fd = open(argv[i], O_RDONLY);
    if (fd < 0) {
      dprintf(2, "grep: %s: %s\n", argv[i], strerror(errno));
      continue;
    }
    if (grep(re, fd, argv[i], show) == 0) rc = 0;
    close(fd);
  }
  return rc;
}
