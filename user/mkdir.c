// mkdir [-p] dir...
#include "user.h"

static int make(char *path, bool parents) {
  if (!parents) {
    if (mkdir(path, 0755) < 0) {
      dprintf(2, "mkdir: %s: %s\n", path, strerror(errno));
      return 1;
    }
    return 0;
  }
  // Create each prefix in turn, ignoring ones that already exist.
  for (char *p = path + 1;; p++) {
    if (*p == '/' || *p == '\0') {
      char saved = *p;
      *p = '\0';
      struct stat st;
      if (mkdir(path, 0755) < 0 && !(errno == EEXIST && stat(path, &st) == 0 && S_ISDIR(st.st_mode))) {
        dprintf(2, "mkdir: %s: %s\n", path, strerror(errno));
        return 1;
      }
      *p = saved;
      if (!saved) break;
    }
  }
  return 0;
}

int main(int argc, char *argv[]) {
  bool parents = false;
  int i = 1;
  if (argc > 1 && strcmp(argv[1], "-p") == 0) {
    parents = true;
    i++;
  }
  if (i >= argc) die("usage: mkdir [-p] dir...");
  int rc = 0;
  for (; i < argc; i++) rc |= make(argv[i], parents);
  return rc;
}
