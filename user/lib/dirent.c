// opendir/readdir/closedir over getdents(2).
#include "user.h"

DIR *opendir(const char *path) {
  int fd = open(path, O_RDONLY | O_DIRECTORY);
  if (fd < 0) return NULL;
  DIR *d = malloc(sizeof(DIR));
  if (!d) {
    close(fd);
    return NULL;
  }
  d->fd = fd;
  d->len = d->pos = 0;
  return d;
}

struct dirent *readdir(DIR *d) {
  if (d->pos >= d->len) {
    int n = getdents(d->fd, d->buf, sizeof(d->buf));
    if (n <= 0) return NULL;
    d->len = n;
    d->pos = 0;
  }
  struct dirent *e = (struct dirent *)(d->buf + d->pos);
  d->pos += e->d_reclen;
  return e;
}

int closedir(DIR *d) {
  int r = close(d->fd);
  free(d);
  return r;
}
