#ifndef SMARTTHINGS_H
#define SMARTTHINGS_H

#include <ArduinoJson.h>
#include <WiFiClientSecure.h>
#include <Logger.h>

struct SpiRamAllocator : ArduinoJson::Allocator 
{
    void* allocate(size_t size) override 
    {
        return heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    }

    void deallocate(void* pointer) override 
    {
        heap_caps_free(pointer);
    }

    void* reallocate(void* ptr, size_t new_size) override 
    {
        return heap_caps_realloc(ptr, new_size, MALLOC_CAP_SPIRAM);
    }
};


class SmartThingsClient
{
    public:
        JsonDocument jsonDoc;

        SmartThingsClient(const char* pat, const char* certificate, ILogger& logger);

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