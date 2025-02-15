#include "EvoHomeClient.h"
#include <Tracer.h>


bool EvoHomeClient::begin(const char* host)
{
    Tracer tracer(F("EvoHomeClient::begin"), host);

    String url = F("http://");
    url += host;
    url += F("/json");

    return RESTClient::begin(url);
}


bool EvoHomeClient::parseResponse(const JsonDocument& response)
{
    zones.clear();
    JsonDocument& doc = const_cast<JsonDocument&>(response);
    JsonArray zoneInfoJsonArray = doc.as<JsonArray>();
    for (JsonVariant zoneInfoJson : zoneInfoJsonArray)
    {
        JsonVariant overrideJson = zoneInfoJson["override"];
        JsonVariant heatDemandJson = zoneInfoJson["heatDemand"];
        ZoneInfo zoneInfo = 
        {
            .setpoint = zoneInfoJson["setpoint"],
            .override = overrideJson.isNull() ? -1 : overrideJson.as<float>(),
            .actual = zoneInfoJson["actual"],
            .heatDemand = heatDemandJson.isNull() ? -1 : heatDemandJson.as<float>(),
            .deviationHours = zoneInfoJson["deviationHours"]
        };
        zones.push_back(zoneInfo);
    }
    
    TRACE("Received zone info for %d zones.\n", zones.size());
    return true;
}
