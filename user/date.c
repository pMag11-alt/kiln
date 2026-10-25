// date: the current UTC time from the RTC.
#include "user.h"

int main(void) {
  long t = time(NULL);
  long days = t / 86400, rem = t % 86400;
  // Civil date from days since 1970-01-01 (Howard Hinnant's algorithm).
  long z = days + 719468;
  long era = (z >= 0 ? z : z - 146096) / 146097;
  long doe = z - era * 146097;
  long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  long mp = (5 * doy + 2) / 153;
  long d = doy - (153 * mp + 2) / 5 + 1;
  long m = mp < 10 ? mp + 3 : mp - 9;
  long y = yoe + era * 400 + (m <= 2);
  static const char *wd[] = {"Thu", "Fri", "Sat", "Sun", "Mon", "Tue", "Wed"};
  static const char *mon[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                              "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  printf("%s %s %2ld %02ld:%02ld:%02ld UTC %ld\n", wd[days % 7], mon[m - 1], d, rem / 3600,
         (rem / 60) % 60, rem % 60, y);
  return 0;
}
