#include "WeatherAPI.h"
#include <WiFiClient.h>
#include <Tracer.h>


bool WeatherAPI::begin(const char* apiKey, const char* location)
{
    Tracer tracer(F("WeatherAPI::begin"), apiKey);

    _filterDoc["liveweer"][0]["temp"] = true;

    String url = F("http://weerlive.nl/api/json-data-10min.php?key=");
    url += apiKey;
    url += F("&locatie=");
    url += location;

    return RESTClient::begin(url);
}


bool WeatherAPI::parseResponse(const JsonDocument& response)
{
    temperature = response["liveweer"][0]["temp"];
    TRACE(F("temperature: %0.1f\n"), temperature);
    return true;
}