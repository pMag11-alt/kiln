// yes [word]: print word (default "y") forever; stops on a broken pipe.
#include "user.h"

int main(int argc, char *argv[]) {
  const char *w = argc > 1 ? argv[1] : "y";
  char buf[4096];
  size_t len = strlen(w), n = 0;
  while (n + len + 1 <= sizeof(buf)) {
    memcpy(buf + n, w, len);
    buf[n + len] = '\n';
    n += len + 1;
  }
  for (;;)
    if (write(1, buf, n) < 0) return 1;
}
