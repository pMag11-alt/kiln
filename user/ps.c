// ps: list processes.
#include "user.h"

int main(void) {
  static struct procinfo procs[64];
  int n = procinfo(procs, 64);
  if (n < 0) die("ps: %s", strerror(errno));
  printf("  PID  PPID  PGID S CPU  MEM(K)   TIME NAME\n");
  for (int i = 0; i < n; i++) {
    struct procinfo *p = &procs[i];
    char state = p->state == PS_SLEEPING ? 'S' : p->state == PS_RUNNABLE ? 'R'
               : p->state == PS_RUNNING ? 'R' : 'Z';
    char cpu[8];
    if (p->cpu >= 0) snprintf(cpu, sizeof(cpu), "%3d", p->cpu);
    else strcpy(cpu, "  -");
    unsigned long secs = (unsigned long)(p->ticks / 100);
    printf("%5d %5d %5d %c %s %7u %3lu:%02lu %s\n", p->pid, p->ppid, p->pgid, state, cpu,
           p->pages * 4, secs / 60, secs % 60, p->name);
  }
  return 0;
}
