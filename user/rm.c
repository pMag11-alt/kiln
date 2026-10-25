// rm [-r] [-f] path...
#include "user.h"

static bool force;

static int remove_path(const char *path, bool recursive) {
  struct stat st;
  if (stat(path, &st) < 0) {
    if (force && errno == ENOENT) return 0;
    dprintf(2, "rm: %s: %s\n", path, strerror(errno));
    return 1;
  }
  if (!S_ISDIR(st.st_mode)) {
    if (unlink(path) < 0) {
      dprintf(2, "rm: %s: %s\n", path, strerror(errno));
      return 1;
    }
    return 0;
  }
  if (!recursive) {
    dprintf(2, "rm: %s: is a directory (use -r)\n", path);
    return 1;
  }
  int rc = 0;
  // Collect names first: deleting while iterating would shift entries.
  for (;;) {
    DIR *d = opendir(path);
    if (!d) {
      dprintf(2, "rm: %s: %s\n", path, strerror(errno));
      return 1;
    }
    char *names[64];
    int n = 0;
    struct dirent *e;
    while (n < 64 && (e = readdir(d))) {
      if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
      names[n++] = strdup(e->d_name);
    }
    closedir(d);
    if (n == 0) break;
    for (int i = 0; i < n; i++) {
      char child[PATH_MAX];
      snprintf(child, sizeof(child), "%s/%s", path, names[i]);
      rc |= remove_path(child, true);
      free(names[i]);
    }
    if (rc) return rc;
  }
  if (rmdir(path) < 0) {
    dprintf(2, "rm: %s: %s\n", path, strerror(errno));
    return 1;
  }
  return rc;
}

int main(int argc, char *argv[]) {
  bool recursive = false;
  int i = 1;
  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
    for (char *f = argv[i] + 1; *f; f++) {
      if (*f == 'r' || *f == 'R') recursive = true;
      else if (*f == 'f') force = true;
      else die("usage: rm [-rf] path...");
    }
  }
  if (i >= argc && !force) die("usage: rm [-rf] path...");
  int rc = 0;
  for (; i < argc; i++) rc |= remove_path(argv[i], recursive);
  return rc;
}
