#ifndef WEATHER_API_H
#define WEATHER_API_H

#include <RESTClient.h>

class WeatherAPI : public RESTClient
{
    public:
        float temperature;

        // Constructor
        WeatherAPI(uint16_t timeout = 15) : RESTClient(timeout) {}

        bool begin(const char* apiKey, const char* location);

    protected:
        virtual bool parseResponse(const JsonDocument& response) override;
};

#endif