#ifndef WIFINTP_H
#define WIFINTP_H

#include <time.h>

class WiFiNTP
{
  public:
    const char* NTPServer = nullptr;

    bool begin(const char* ntpServer, const char* timeZone = nullptr);

    bool beginGetServerTime();
    time_t endGetServerTime();
    time_t getServerTime();
    time_t getCurrentTime();

  private:
    bool _isInitialized = false;
};

#endif