// primes [N]: a concurrent prime sieve, one process per prime, connected
// by pipes (after Doug McIlroy).
#include "user.h"

static void sieve(int in) __attribute__((noreturn));

static void sieve(int in) {
  int p;
  if (read(in, &p, sizeof(p)) != sizeof(p)) exit(0);
  printf("%d\n", p);
  int fds[2];
  if (pipe(fds) < 0) die("primes: pipe: %s", strerror(errno));
  pid_t pid = fork();
  if (pid < 0) die("primes: fork: %s", strerror(errno));
  if (pid == 0) {
    close(fds[1]);
    close(in);
    sieve(fds[0]);
  }
  close(fds[0]);
  int n;
  while (read(in, &n, sizeof(n)) == sizeof(n))
    if (n % p) write(fds[1], &n, sizeof(n));
  close(fds[1]);
  close(in);
  wait(NULL);
  exit(0);
}

int main(int argc, char *argv[]) {
  int limit = argc > 1 ? atoi(argv[1]) : 100;
  int fds[2];
  if (pipe(fds) < 0) die("primes: pipe: %s", strerror(errno));
  pid_t pid = fork();
  if (pid == 0) {
    close(fds[1]);
    sieve(fds[0]);
  }
  close(fds[0]);
  for (int i = 2; i <= limit; i++) write(fds[1], &i, sizeof(i));
  close(fds[1]);
  wait(NULL);
  return 0;
}
