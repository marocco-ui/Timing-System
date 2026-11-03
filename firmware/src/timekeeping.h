#pragma once
// Pure time math, no Arduino dependencies, so it can be unit-tested on a PC.
#include <stdint.h>

// Days since 1970-01-01 for a proleptic Gregorian date.
// Howard Hinnant's days_from_civil algorithm.
static inline int64_t daysFromCivil(int32_t y, uint32_t m, uint32_t d) {
  y -= m <= 2;
  const int32_t era = (y >= 0 ? y : y - 399) / 400;
  const uint32_t yoe = (uint32_t)(y - era * 400);
  const uint32_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const uint32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return (int64_t)era * 146097 + (int64_t)doe - 719468;
}

// Seconds since the Unix epoch for a UTC date and time.
static inline int64_t utcToEpoch(int32_t year, uint32_t month, uint32_t day,
                                 uint32_t hour, uint32_t minute, uint32_t second) {
  return daysFromCivil(year, month, day) * 86400LL +
         (int64_t)hour * 3600 + (int64_t)minute * 60 + second;
}

// A GPS anchor ties a local microsecond timestamp (taken at a PPS edge) to
// the UTC second that edge marks.
struct Anchor {
  bool valid;
  int64_t ppsLocalUs;   // esp_timer_get_time() at the PPS edge
  int64_t epochSec;     // UTC second that PPS edge marks
};

// UTC microseconds for an event captured at localUs.
static inline int64_t eventUtcUs(const Anchor &a, int64_t localUs) {
  return a.epochSec * 1000000LL + (localUs - a.ppsLocalUs);
}

// How far (ms) the event is from the anchor. Large values mean the box was
// coasting on its own crystal instead of fresh GPS time.
static inline int32_t syncAgeMs(const Anchor &a, int64_t localUs) {
  int64_t d = localUs - a.ppsLocalUs;
  if (d < 0) d = -d;
  return (int32_t)(d / 1000);
}
