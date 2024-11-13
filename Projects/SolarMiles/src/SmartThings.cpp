#include <Tracer.h>
#include <HTTPClient.h>
#include "SmartThings.h"

const char* _rootCA = "-----BEGIN CERTIFICATE-----\n" \
"MIIDQTCCAimgAwIBAgITBmyfz5m/jAo54vB4ikPmljZbyjANBgkqhkiG9w0BAQsF\n" \
"ADA5MQswCQYDVQQGEwJVUzEPMA0GA1UEChMGQW1hem9uMRkwFwYDVQQDExBBbWF6\n" \
"b24gUm9vdCBDQSAxMB4XDTE1MDUyNjAwMDAwMFoXDTM4MDExNzAwMDAwMFowOTEL\n" \
"MAkGA1UEBhMCVVMxDzANBgNVBAoTBkFtYXpvbjEZMBcGA1UEAxMQQW1hem9uIFJv\n" \
"b3QgQ0EgMTCCASIwDQYJKoZIhvcNAQEBBQADggEPADCCAQoCggEBALJ4gHHKeNXj\n" \
"ca9HgFB0fW7Y14h29Jlo91ghYPl0hAEvrAIthtOgQ3pOsqTQNroBvo3bSMgHFzZM\n" \
"9O6II8c+6zf1tRn4SWiw3te5djgdYZ6k/oI2peVKVuRF4fn9tBb6dNqcmzU5L/qw\n" \
"IFAGbHrQgLKm+a/sRxmPUDgH3KKHOVj4utWp+UhnMJbulHheb4mjUcAwhmahRWa6\n" \
"VOujw5H5SNz/0egwLX0tdHA114gk957EWW67c4cX8jJGKLhD+rcdqsq08p8kDi1L\n" \
"93FcXmn/6pUCyziKrlA4b9v7LWIbxcceVOF34GfID5yHI9Y/QCB/IIDEgEw+OyQm\n" \
"jgSubJrIqg0CAwEAAaNCMEAwDwYDVR0TAQH/BAUwAwEB/zAOBgNVHQ8BAf8EBAMC\n" \
"AYYwHQYDVR0OBBYEFIQYzIU07LwMlJQuCFmcx7IQTgoIMA0GCSqGSIb3DQEBCwUA\n" \
"A4IBAQCY8jdaQZChGsV2USggNiMOruYou6r4lK5IpDB/G/wkjUu0yKGX9rbxenDI\n" \
"U5PMCCjjmCXPI6T53iHTfIUJrU6adTrCC2qJeHZERxhlbI1Bjjt/msv0tadQ1wUs\n" \
"N+gDS63pYaACbvXy8MWy7Vu33PqUXHeeE6V/Uq2V8viTO96LXFvKWlJbYK8U90vv\n" \
"o/ufQJVtMVT8QtPHRh8jrdkPSHCa2XV4cdFyQzR1bldZwgJcJmApzyMZFo6IQ6XU\n" \
"5MsI+yMRQ+hDKXJioaldXgjUkK642M4UwtBV8ob2xJNDd2ZhwLnoQdeXeGADbkpy\n" \
"rqXRfboQnoZsG4q5WTP468SQvvG5\n" \
"-----END CERTIFICATE-----\n";

SmartThingsClient::SmartThingsClient(const char* pat, ILogger& logger)
    : jsonDoc(&_spiRamAllocator), _logger(logger) 
{
    _pat = pat;
    _wifiClientSecure.setTimeout(5);
    _wifiClientSecure.setCACert(_rootCA);

    _devicesFilter["items"][0]["deviceId"] = true;
    _devicesFilter["items"][0]["label"] = true;
    _devicesFilter["items"][0]["components"][0]["id"] = true;
    _devicesFilter["items"][0]["components"][0]["capabilities"][0]["id"] = true;
 
    _deviceStatusFilter["switch"]["switch"]["value"] = true;
    _deviceStatusFilter["powerMeter"]["power"]["value"] = true;
    _deviceStatusFilter["energyMeter"]["energy"]["value"] = true;
    _deviceStatusFilter["energyMeter"]["energy"]["unit"] = true;
    _deviceStatusFilter["powerConsumptionReport"]["powerConsumption"]["value"]["power"] = true;
    _deviceStatusFilter["powerConsumptionReport"]["powerConsumption"]["value"]["energy"] = true;
    _deviceStatusFilter["powerConsumptionReport"]["powerConsumption"]["value"]["deltaEnergy"] = true;
    _deviceStatusFilter["powerConsumptionReport"]["powerConsumption"]["timestamp"] = true;
    _deviceStatusFilter["samsungce.dishwasherOperation"]["remainingTimeStr"]["value"] = true;
    _deviceStatusFilter["samsungce.dishwasherWashingCourse"]["washingCourse"]["value"] = true;
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

    httpClient.end();

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
