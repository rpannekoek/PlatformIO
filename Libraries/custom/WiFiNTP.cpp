#include <WiFiNTP.h>
#include <Tracer.h>

bool WiFiNTP::begin(const char* ntpServer, const char* timezone)
{
    Tracer tracer(F("WiFiNTP::begin"), ntpServer);

    if (timezone == nullptr)
        timezone = "CET-1CEST,M3.5.0,M10.5.0/3"; // Amsterdam TZ
        
#ifdef ESP8266
    configTime(timezone, ntpServer);
#else
    configTzTime(timezone, ntpServer);
#endif

    NTPServer = ntpServer;
    _isInitialized = true;
    return true;
}


bool WiFiNTP::beginGetServerTime()
{
    Tracer tracer(F("WiFiNTP::beginGetServerTime"));

    return _isInitialized
        ? true
        : begin(NTPServer); // For backwards compatibility
}


time_t WiFiNTP::endGetServerTime()
{
    time_t currentTime = time(nullptr);
    return (currentTime < 100000) ? (time_t)0 : currentTime;
}


time_t WiFiNTP::getServerTime()
{
    Tracer tracer(F("WiFiNTP::getServerTime"));

    if (!beginGetServerTime())
        return 0;

    TRACE(F("Awaiting NTP server response..."));
    for (int i = 0; i < 20; i++)
    {
        time_t result = endGetServerTime();
        if (result != 0)
            return result;
        TRACE(".");
        delay(100);
    }

    TRACE(F("\nTimeout waiting for NTP Server response.\n"));
    return 0;
}


time_t WiFiNTP::getCurrentTime()
{
    return time(nullptr);
}
