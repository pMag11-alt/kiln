// shutdown [-r]: power off (or reboot) the machine.
#include "user.h"

int main(int argc, char *argv[]) {
  bool reboot = argc > 1 && strcmp(argv[1], "-r") == 0;
  shutdown(reboot ? SHUTDOWN_REBOOT : SHUTDOWN_POWEROFF);
}
