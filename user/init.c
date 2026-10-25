// init: create the device nodes, run /etc/rc, then keep a shell alive on the
// console and reap orphaned processes.
#include "user.h"

static void ensure_dev(const char *path, int major, int minor) {
  struct stat st;
  if (stat(path, &st) == 0) return;
  mknod(path, S_IFCHR | 0666, MKDEV(major, minor));
}

static void cat_file(const char *path) {
  int fd = open(path, O_RDONLY);
  if (fd < 0) return;
  char buf[512];
  ssize_t n;
  while ((n = read(fd, buf, sizeof(buf))) > 0) write(1, buf, (size_t)n);
  close(fd);
}

static int run(char *const argv[]) {
  pid_t pid = fork();
  if (pid < 0) return -1;
  if (pid == 0) {
    setpgid(0, 0);
    execv(argv[0], argv);
    dprintf(2, "init: cannot exec %s: %s\n", argv[0], strerror(errno));
    exit(127);
  }
  int status;
  while (wait(&status) != pid) {}
  return status;
}

int main(void) {
  mkdir("/dev", 0755);
  ensure_dev("/dev/console", MAJOR_CONSOLE, 1);
  ensure_dev("/dev/null", MAJOR_MEM, MINOR_NULL);
  ensure_dev("/dev/zero", MAJOR_MEM, MINOR_ZERO);
  if (open("/dev/console", O_RDWR) < 0) exit(1);
  dup(0);   // stdout
  dup(0);   // stderr
  mkdir("/tmp", 01777);

  struct stat st;
  if (stat("/etc/rc", &st) == 0) {
    char *rc[] = {"/bin/sh", "/etc/rc", NULL};
    run(rc);
  }
  cat_file("/etc/motd");

  for (;;) {
    pid_t sh = fork();
    if (sh < 0) {
      dprintf(2, "init: fork failed: %s\n", strerror(errno));
      msleep(1000);
      continue;
    }
    if (sh == 0) {
      char *argv[] = {"sh", NULL};
      execv("/bin/sh", argv);
      dprintf(2, "init: cannot exec /bin/sh: %s\n", strerror(errno));
      exit(127);
    }
    // Reap everything: our shell and any orphans re-parented to us.
    for (;;) {
      int status;
      pid_t w = wait(&status);
      if (w == sh || w < 0) break;
    }
    printf("init: shell exited, starting a new one\n");
  }
}
