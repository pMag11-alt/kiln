// Goldfish real-time clock (QEMU virt): nanoseconds since the Unix epoch.
#include "defs.h"

#define RTC_TIME_LOW  0x00
#define RTC_TIME_HIGH 0x04

u64 rtc_now(void) {
  if (!plat.rtc_base) return 0;
  volatile u32 *rtc = P2V(plat.rtc_base);
  // Reading TIME_LOW latches TIME_HIGH, so low must be read first.
  u64 lo = rtc[RTC_TIME_LOW / 4];
  u64 hi = rtc[RTC_TIME_HIGH / 4];
  return ((hi << 32) | lo) / 1000000000UL;
}
