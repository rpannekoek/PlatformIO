#ifndef SMARTTHINGS_H
#define SMARTTHINGS_H

#include <ArduinoJson.h>
#include <WiFiClientSecure.h>
#include <Logger.h>
#include <SpiRamAllocator.h>

class SmartThingsClient
{
    public:
        JsonDocument jsonDoc;

        SmartThingsClient(const char* pat, ILogger& logger);

        uint32_t responseTimeMs() { return _responseTimeMs; }

        bool request(const String& urlPath, const JsonDocument& filterDoc);
        bool requestDevices();
        bool requestDeviceStatus(const String& deviceId);
        void cleanup();

    private:
        WiFiClientSecure _wifiClientSecure;
        SpiRamAllocator _spiRamAllocator;
        ILogger& _logger;
        String _pat;
        uint32_t _responseTimeMs = 0;
        JsonDocument _devicesFilter;
        JsonDocument _deviceStatusFilter;
};

#endif