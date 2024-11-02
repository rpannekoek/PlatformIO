#include <Tracer.h>
#include <HTTPClient.h>
#include "SmartThings.h"

SmartThingsClient::SmartThingsClient(const char* pat, const char* certificate, ILogger& logger)
    : jsonDoc(&_spiRamAllocator), _logger(logger) 
{
    _pat = pat;
    _wifiClientSecure.setTimeout(5);
    _wifiClientSecure.setCACert(certificate);

    _devicesFilter["items"][0]["deviceId"] = true;
    _devicesFilter["items"][0]["label"] = true;
    _devicesFilter["items"][0]["components"][0]["id"] = true;
    _devicesFilter["items"][0]["components"][0]["capabilities"][0]["id"] = true;
 
    _deviceStatusFilter["switch"]["switch"]["value"] = true;
    _deviceStatusFilter["powerMeter"]["power"]["value"] = true;
    _deviceStatusFilter["energyMeter"]["energy"]["value"] = true;
    _deviceStatusFilter["energyMeter"]["energy"]["unit"] = true;
}


bool SmartThingsClient::request(const String& urlPath, const JsonDocument& filterDoc)
{
    Tracer tracer("SmartThingsClient::request", urlPath.c_str());

    String url = "https://api.smartthings.com/v1";
    url += urlPath;

    HTTPClient httpClient;
    if (!httpClient.begin(_wifiClientSecure, url))
    {
        _logger.logEvent("SmartThings: HTTPClient.begin failed.");
        return false;
    }

    String bearerToken = "Bearer ";
    bearerToken += _pat;
    httpClient.addHeader("Authorization", bearerToken);

    uint32_t startMillis = millis();
    int code = httpClient.GET();
    _responseTimeMs = millis() - startMillis;

    if (code < 0)
    {
        _logger.logEvent("SmartThings: %s", httpClient.errorToString(code).c_str());
        return false;
    }
    if (code != HTTP_CODE_OK)
    {
        _logger.logEvent("SmartThings: HTTP code %d", code);
        return false;
    }

    TRACE("HTTP Response size: %d\n", httpClient.getSize());

    DeserializationError parseError = deserializeJson(
        jsonDoc,
        httpClient.getStream(),
        DeserializationOption::Filter(filterDoc));

    if (parseError != DeserializationError::Ok)
    {
        _logger.logEvent("SmartThings: JSON error %s", parseError.c_str());
        return false;
    }

    return true;
}


bool SmartThingsClient::requestDevices()
{
    return request("/devices", _devicesFilter);
}


bool SmartThingsClient::requestDeviceStatus(const String& deviceId)
{
    String urlPath = "/devices/";
    urlPath += deviceId;
    urlPath += "/components/main/status";

    return request(urlPath, _deviceStatusFilter);
}


void SmartThingsClient::cleanup()
{
    jsonDoc.clear();
    jsonDoc.shrinkToFit();
}
