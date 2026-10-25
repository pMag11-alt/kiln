// Spinlocks and sleeplocks.
#include "defs.h"

void initlock(struct spinlock *lk, const char *name) {
  lk->locked = 0;
  lk->name = name;
  lk->cpu = NULL;
}

// Interrupts stay off while any spinlock is held, so an interrupt handler
// can never spin on a lock its own hart holds.
void acquire(struct spinlock *lk) {
  push_off();
  if (holding(lk)) panic("acquire: %s already held", lk->name);
  while (__atomic_exchange_n(&lk->locked, 1, __ATOMIC_ACQUIRE)) {}
  lk->cpu = mycpu();
}

void release(struct spinlock *lk) {
  if (!holding(lk)) panic("release: %s not held", lk->name);
  lk->cpu = NULL;
  __atomic_store_n(&lk->locked, 0, __ATOMIC_RELEASE);
  pop_off();
}

bool holding(struct spinlock *lk) {
  return lk->locked && lk->cpu == mycpu();
}

// push_off/pop_off are like intr_off/intr_on but nest: interrupts come back
// on only when the outermost pop_off runs and they were on before.
void push_off(void) {
  bool old = intr_get();
  intr_off();
  struct cpu *c = mycpu();
  if (c->noff == 0) c->intena = old;
  c->noff++;
}

void pop_off(void) {
  struct cpu *c = mycpu();
  if (intr_get()) panic("pop_off: interrupts enabled");
  if (c->noff < 1) panic("pop_off: unbalanced");
  c->noff--;
  if (c->noff == 0 && c->intena) intr_on();
}

void initsleeplock(struct sleeplock *lk, const char *name) {
  initlock(&lk->lk, "sleeplock");
  lk->name = name;
  lk->locked = false;
  lk->pid = 0;
}

void acquiresleep(struct sleeplock *lk) {
  acquire(&lk->lk);
  while (lk->locked) sleep(lk, &lk->lk);
  lk->locked = true;
  lk->pid = myproc() ? myproc()->pid : -1;
  release(&lk->lk);
}

void releasesleep(struct sleeplock *lk) {
  acquire(&lk->lk);
  lk->locked = false;
  lk->pid = 0;
  wakeup(lk);
  release(&lk->lk);
}

bool holdingsleep(struct sleeplock *lk) {
  acquire(&lk->lk);
  bool r = lk->locked && lk->pid == (myproc() ? myproc()->pid : -1);
  release(&lk->lk);
  return r;
}
