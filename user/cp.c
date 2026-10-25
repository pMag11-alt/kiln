// cp src dst  |  cp src... dir
#include "user.h"

static int copy_file(const char *src, const char *dst) {
  int in = open(src, O_RDONLY);
  if (in < 0) {
    dprintf(2, "cp: %s: %s\n", src, strerror(errno));
    return 1;
  }
  struct stat st;
  fstat(in, &st);
  if (S_ISDIR(st.st_mode)) {
    dprintf(2, "cp: %s: is a directory\n", src);
    close(in);
    return 1;
  }
  int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, (int)(st.st_mode & 0777));
  if (out < 0) {
    dprintf(2, "cp: %s: %s\n", dst, strerror(errno));
    close(in);
    return 1;
  }
  char buf[8192];
  ssize_t n;
  int rc = 0;
  while ((n = read(in, buf, sizeof(buf))) > 0) {
    if (write(out, buf, (size_t)n) != n) {
      dprintf(2, "cp: %s: %s\n", dst, strerror(errno));
      rc = 1;
      break;
    }
  }
  if (n < 0) rc = 1;
  close(in);
  close(out);
  return rc;
}

int main(int argc, char *argv[]) {
  if (argc < 3) die("usage: cp src dst | cp src... dir");
  const char *dst = argv[argc - 1];
  struct stat st;
  bool into = stat(dst, &st) == 0 && S_ISDIR(st.st_mode);
  if (argc > 3 && !into) die("cp: %s is not a directory", dst);
  int rc = 0;
  for (int i = 1; i < argc - 1; i++) {
    char target[PATH_MAX];
    if (into) snprintf(target, sizeof(target), "%s/%s", dst, basename_of(argv[i]));
    else strlcpy(target, dst, sizeof(target));
    rc |= copy_file(argv[i], target);
  }
  return rc;
}
