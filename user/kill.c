// kill [-SIG] pid...   (SIG is a number or INT, KILL, TERM, SEGV)
#include "user.h"

int main(int argc, char *argv[]) {
  int sig = SIGTERM;
  int i = 1;
  if (i < argc && argv[i][0] == '-' && argv[i][1] && !isdigit(argv[i][2]) && isalpha(argv[i][1])) {
    const char *s = argv[i] + 1;
    if (!strcmp(s, "INT")) sig = SIGINT;
    else if (!strcmp(s, "KILL")) sig = SIGKILL;
    else if (!strcmp(s, "TERM")) sig = SIGTERM;
    else if (!strcmp(s, "SEGV")) sig = SIGSEGV;
    else die("kill: unknown signal %s", s);
    i++;
  } else if (i < argc && argv[i][0] == '-' && isdigit(argv[i][1])) {
    sig = atoi(argv[i] + 1);
    i++;
  }
  if (i >= argc) die("usage: kill [-SIG] pid...");
  int rc = 0;
  for (; i < argc; i++) {
    if (kill(atoi(argv[i]), sig) < 0) {
      dprintf(2, "kill: %s: %s\n", argv[i], strerror(errno));
      rc = 1;
    }
  }
  return rc;
}
