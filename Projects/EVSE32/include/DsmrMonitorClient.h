#ifndef DSMRMON_CLIENT_H
#define DSMRMON_CLIENT_H

#include <WiFiClient.h>
#include <HTTPClient.h>

struct PhaseData
{
    String Name;
    float Voltage;
    float Current;
    float Power;

    PhaseData()
    {
    }

    PhaseData(const PhaseData& other)
    {
        Name = other.Name;
        Voltage = other.Voltage;
        Current = other.Current;
        Power = other.Power;
    }
};

class DsmrMonitorClient
{
    public:
        bool isInitialized;
        std::vector<PhaseData> electricity;

        // Constructor
        DsmrMonitorClient(uint16_t timeout = 5000); // 5 seconds

        bool begin(const char* host);
        int awaitData();

        String getLastError() { return _lastError; }

    private:
        WiFiClient _wifiClient;
        HTTPClient _httpClient;
        String _lastError;

        bool parseJson(String json);
};


#endif