#ifndef LOGGER_H
#define LOGGER_H

#include <WString.h>

class ILogger
{
    public:
        virtual void logEvent(const char* msg) = 0;
        virtual void logEvent(String format, ...) = 0;
};

#endif