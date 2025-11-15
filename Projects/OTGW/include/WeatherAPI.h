#ifndef WEATHER_API_H
#define WEATHER_API_H

#include <RESTClient.h>

class WeatherAPI : public RESTClient
{
    public:
        float temperature;

        WeatherAPI(uint16_t timeout = 15, MemoryType memoryType = MemoryType::Internal)
            : RESTClient(timeout, memoryType) {}

        bool begin(const char* apiKey, const char* location);

    protected:
        virtual bool parseResponse(const JsonDocument& response) override;
};

#endif