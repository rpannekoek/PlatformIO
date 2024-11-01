#ifndef TIMEUTILS_H
#define TIMEUTILS_H

#include <time.h>

constexpr int SECONDS_PER_MINUTE = 60;
constexpr int SECONDS_PER_HOUR = 60 * SECONDS_PER_MINUTE;
constexpr int SECONDS_PER_DAY = 24 * SECONDS_PER_HOUR;
constexpr int SECONDS_PER_WEEK = 7 * SECONDS_PER_DAY;

extern const char* formatTime(const char* format, time_t time);
extern const char* formatTimeSpan(uint32_t seconds, bool includeHours = true);
extern time_t getStartOfDay(time_t time);

#endif