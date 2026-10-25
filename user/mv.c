// mv src dst  |  mv src... dir
#include "user.h"

static bool is_dir(const char *path) {
  struct stat st;
  return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

int main(int argc, char *argv[]) {
  if (argc < 3) die("usage: mv src dst | mv src... dir");
  const char *dst = argv[argc - 1];
  bool into = is_dir(dst);
  if (argc > 3 && !into) die("mv: %s is not a directory", dst);
  int rc = 0;
  for (int i = 1; i < argc - 1; i++) {
    char target[PATH_MAX];
    if (into) snprintf(target, sizeof(target), "%s/%s", dst, basename_of(argv[i]));
    else strlcpy(target, dst, sizeof(target));
    if (rename(argv[i], target) < 0) {
      dprintf(2, "mv: %s -> %s: %s\n", argv[i], target, strerror(errno));
      rc = 1;
    }
  }
  return rc;
}
