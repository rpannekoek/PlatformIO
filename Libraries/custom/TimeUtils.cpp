#include <stdio.h>
#include "TimeUtils.h"

const char* formatTime(const char* format, time_t time)
{
    static char result[32];
    strftime(result, sizeof(result), format, localtime(&time));
    return result;
}

const char* formatTimeSpan(uint32_t seconds, bool includeHours)
{
    static char result[16];
    if (includeHours)
    {
        snprintf(
            result,
            sizeof(result),
            "%02lu:%02lu:%02lu",
            seconds / 3600,
            (seconds / 60) % 60,
            seconds % 60);
    }
    else
    {
        snprintf(
            result,
            sizeof(result),
            "%02lu:%02lu",
            seconds / 60,
            seconds % 60);
    }
    return result;
}


time_t getStartOfDay(time_t time)
{
    tm* tmPtr = localtime(&time);
    tmPtr->tm_hour = 0;
    tmPtr->tm_min = 0;
    tmPtr->tm_sec = 0;
    return mktime(tmPtr);
}
