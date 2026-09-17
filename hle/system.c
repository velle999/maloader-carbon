// Copyright 2026 Velle Sinclair.
//
// Simplified BSD License or GPLv3, like the rest of this tree.

// Dates, clocks and Gestalt.
//
// Classic dates count local-time seconds from 1904. AbsoluteTime is an
// UnsignedWide, low word first on Intel, so it is a plain 64-bit value in
// nanoseconds here, the units mach_absolute_time uses. Returned from a
// function, Darwin puts that 8-byte struct in EAX:EDX, which is where a
// uint64_t goes.

#define _GNU_SOURCE

#include <string.h>
#include <sys/sysinfo.h>
#include <time.h>

#include "carbon.h"

typedef struct {
  SInt16 year;
  SInt16 month;
  SInt16 day;
  SInt16 hour;
  SInt16 minute;
  SInt16 second;
  // 1 is Sunday.
  SInt16 dayOfWeek;
} DateTimeRec;

typedef struct {
  SInt16 era;
  SInt16 year;
  SInt16 month;
  SInt16 day;
  SInt16 hour;
  SInt16 minute;
  SInt16 second;
  SInt16 dayOfWeek;
  SInt16 dayOfYear;
  SInt16 weekOfYear;
  SInt16 pm;
  SInt16 res1;
  SInt16 res2;
  SInt16 res3;
} LongDateRec;

_Static_assert(sizeof(DateTimeRec) == 14, "DateTimeRec");
_Static_assert(sizeof(LongDateRec) == 28, "LongDateRec");

// The broken-down date of |seconds| since 1904, taken as already local.
static void date_of(int64_t seconds, struct tm* tm) {
  time_t t = (time_t)(seconds - (int64_t)HLE_MAC_EPOCH_OFFSET);
  gmtime_r(&t, tm);
}

void GetDateTime(UInt32* seconds) {
  *seconds = hle_mac_local_seconds(time(NULL));
}

void SecondsToDate(UInt32 seconds, DateTimeRec* date) {
  struct tm tm;
  date_of(seconds, &tm);
  date->year = tm.tm_year + 1900;
  date->month = tm.tm_mon + 1;
  date->day = tm.tm_mday;
  date->hour = tm.tm_hour;
  date->minute = tm.tm_min;
  date->second = tm.tm_sec;
  date->dayOfWeek = tm.tm_wday + 1;
}

void GetTime(DateTimeRec* date) {
  UInt32 now;
  GetDateTime(&now);
  SecondsToDate(now, date);
}

void LongSecondsToDate(const int64_t* seconds, LongDateRec* date) {
  struct tm tm;
  date_of(*seconds, &tm);
  memset(date, 0, sizeof(*date));
  date->year = tm.tm_year + 1900;
  date->month = tm.tm_mon + 1;
  date->day = tm.tm_mday;
  date->hour = tm.tm_hour;
  date->minute = tm.tm_min;
  date->second = tm.tm_sec;
  date->dayOfWeek = tm.tm_wday + 1;
  date->dayOfYear = tm.tm_yday + 1;
  date->weekOfYear = (tm.tm_yday + 7 - tm.tm_wday) / 7 + 1;
  date->pm = tm.tm_hour >= 12;
}

void LongDateToSeconds(const LongDateRec* date, int64_t* seconds) {
  struct tm tm;
  memset(&tm, 0, sizeof(tm));
  tm.tm_year = date->year - 1900;
  tm.tm_mon = date->month - 1;
  tm.tm_mday = date->day;
  tm.tm_hour = date->hour;
  tm.tm_min = date->minute;
  tm.tm_sec = date->second;
  *seconds = (int64_t)timegm(&tm) + HLE_MAC_EPOCH_OFFSET;
}

int ConvertLocalTimeToUTC(UInt32 local, UInt32* utc) {
  time_t t = (time_t)((int64_t)local - (int64_t)HLE_MAC_EPOCH_OFFSET);
  struct tm tm;
  localtime_r(&t, &tm);
  *utc = local - (UInt32)tm.tm_gmtoff;
  return noErr;
}

// ---------------------------------------------------------------------------
// Clocks

static uint64_t monotonic_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000u + ts.tv_nsec;
}

// Sixtieths of a second.
UInt32 TickCount(void) {
  return (UInt32)(monotonic_ns() * 60 / 1000000000u);
}

uint64_t UpTime(void) {
  return monotonic_ns();
}

// An UnsignedWide, low word first: a 64-bit count of microseconds.
void Microseconds(uint64_t* microseconds) {
  *microseconds = monotonic_ns() / 1000u;
}

// A Duration counts milliseconds when positive, microseconds when negative.
uint64_t DurationToAbsolute(int32_t duration) {
  return duration >= 0 ? (uint64_t)duration * 1000000u
                       : (uint64_t)(-(int64_t)duration) * 1000u;
}

uint64_t AddDurationToAbsolute(int32_t duration, uint64_t absolute) {
  return absolute + DurationToAbsolute(duration);
}

uint64_t AbsoluteToNanoseconds(uint64_t absolute) {
  return absolute;
}

uint64_t NanosecondsToAbsolute(uint64_t nanoseconds) {
  return nanoseconds;
}

// ---------------------------------------------------------------------------
// Gestalt: the selectors the game asks for.

enum {
  gestaltUndefSelectorErr = -5551,
};

int Gestalt(OSType selector, int32_t* response) {
  switch (selector) {
    case 'sysv':
      // Mac OS X 10.4.9, as the 'sysv' encoding caps the last digits at 9.
      *response = 0x1049;
      break;
    case 'ramm': {
      struct sysinfo info;
      *response = sysinfo(&info) == 0
                      ? (int32_t)((uint64_t)info.totalram * info.mem_unit >> 20)
                      : 512;
      break;
    }
    case 'qtim':
      // QuickTime 7.1.6.
      *response = 0x07168000;
      break;
    case 'evnt':
      // gestaltAppleEventsPresent.
      *response = 1;
      break;
    case 'dply':
      // gestaltDisplayMgrPresent and gestaltDisplayMgrSetDepthNotifies.
      *response = 0x3;
      break;
    default:
      cf_trace("Gestalt('%c%c%c%c'): unknown selector",
               (int)(selector >> 24) & 0xff, (int)(selector >> 16) & 0xff,
               (int)(selector >> 8) & 0xff, (int)selector & 0xff);
      return gestaltUndefSelectorErr;
  }
  cf_trace("Gestalt('%c%c%c%c') = %#x", (int)(selector >> 24) & 0xff,
           (int)(selector >> 16) & 0xff, (int)(selector >> 8) & 0xff,
           (int)selector & 0xff, *response);
  return noErr;
}
