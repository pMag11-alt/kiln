// ls [-l] [-a] [path...]
#include "user.h"

static bool longfmt, all;

struct entry {
  char *name;
  struct stat st;
};

static int cmp_entry(const void *a, const void *b) {
  return strcmp(((const struct entry *)a)->name, ((const struct entry *)b)->name);
}

static void mode_string(uint32_t m, char *s) {
  s[0] = S_ISDIR(m) ? 'd' : S_ISCHR(m) ? 'c' : S_ISLNK(m) ? 'l' : S_ISFIFO(m) ? 'p' : '-';
  const char *rwx = "rwxrwxrwx";
  for (int i = 0; i < 9; i++) s[1 + i] = (m & (0400 >> i)) ? rwx[i] : '-';
  s[10] = '\0';
}

// Civil date from days since 1970-01-01 (Howard Hinnant's algorithm).
static void civil(long days, int *y, int *mo, int *d) {
  days += 719468;
  long era = (days >= 0 ? days : days - 146096) / 146097;
  long doe = days - era * 146097;
  long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  long mp = (5 * doy + 2) / 153;
  *d = (int)(doy - (153 * mp + 2) / 5 + 1);
  *mo = (int)(mp < 10 ? mp + 3 : mp - 9);
  *y = (int)(yoe + era * 400 + (*mo <= 2));
}

static void print_entry(const struct entry *e) {
  if (!longfmt) {
    printf("%s%s\n", e->name, S_ISDIR(e->st.st_mode) ? "/" : "");
    return;
  }
  static const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                 "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  char mode[11];
  mode_string(e->st.st_mode, mode);
  int y, mo, d;
  long t = e->st.st_mtime;
  civil(t / 86400, &y, &mo, &d);
  if (S_ISCHR(e->st.st_mode)) {
    printf("%s %2u %3u, %3u %s %2d %4d %s\n", mode, e->st.st_nlink, DEV_MAJOR(e->st.st_rdev),
           DEV_MINOR(e->st.st_rdev), months[mo - 1], d, y, e->name);
  } else {
    printf("%s %2u %8lu %s %2d %4d %02ld:%02ld %s%s\n", mode, e->st.st_nlink,
           (unsigned long)e->st.st_size, months[mo - 1], d, y, (t / 3600) % 24, (t / 60) % 60,
           e->name, S_ISDIR(e->st.st_mode) ? "/" : "");
  }
}

static int list(const char *path, bool header) {
  struct stat st;
  if (stat(path, &st) < 0) {
    dprintf(2, "ls: %s: %s\n", path, strerror(errno));
    return 1;
  }
  if (!S_ISDIR(st.st_mode)) {
    struct entry e = {(char *)path, st};
    print_entry(&e);
    return 0;
  }
  DIR *d = opendir(path);
  if (!d) {
    dprintf(2, "ls: %s: %s\n", path, strerror(errno));
    return 1;
  }
  int cap = 64, n = 0;
  struct entry *ents = malloc((size_t)cap * sizeof(*ents));
  struct dirent *de;
  while ((de = readdir(d))) {
    if (!all && de->d_name[0] == '.') continue;
    if (n == cap) {
      cap *= 2;
      ents = realloc(ents, (size_t)cap * sizeof(*ents));
    }
    ents[n].name = strdup(de->d_name);
    char full[PATH_MAX];
    snprintf(full, sizeof(full), "%s/%s", path, de->d_name);
    if (stat(full, &ents[n].st) < 0) memset(&ents[n].st, 0, sizeof(ents[n].st));
    n++;
  }
  closedir(d);
  qsort(ents, (size_t)n, sizeof(*ents), cmp_entry);
  if (header) printf("%s:\n", path);
  for (int i = 0; i < n; i++) {
    print_entry(&ents[i]);
    free(ents[i].name);
  }
  free(ents);
  return 0;
}

int main(int argc, char *argv[]) {
  int i = 1;
  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
    for (char *f = argv[i] + 1; *f; f++) {
      if (*f == 'l') longfmt = true;
      else if (*f == 'a') all = true;
      else if (*f != '1') die("usage: ls [-la] [path...]");
    }
  }
  if (i == argc) return list(".", false);
  int rc = 0;
  bool many = argc - i > 1;
  for (; i < argc; i++) {
    rc |= list(argv[i], many);
    if (many && i + 1 < argc) printf("\n");
  }
  return rc;
}
