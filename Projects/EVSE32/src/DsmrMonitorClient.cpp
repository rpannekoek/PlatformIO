#include <ArduinoJson.h>
#include <StreamString.h>
#include <Tracer.h>
#include "DsmrMonitorClient.h"

#define MIN_CONTENT_LENGTH 100

JsonDocument _response;

// Constructor
DsmrMonitorClient::DsmrMonitorClient(uint16_t timeout)
{
    isInitialized = false;
    _httpClient.setTimeout(timeout);
     // Re-use TCP connection (using HTTP Keep-Alive)?
     // ESP8266 WebServer Keep-Alive times out after 2 sec, so it's useless to request it.
    _httpClient.setReuse(false);
}

bool DsmrMonitorClient::begin(const char* host)
{
    Tracer tracer("DsmrMonitorClient::begin", host);

    bool result = _httpClient.begin(_wifiClient, host, 80, "/json");
    if (!result)
        _lastError = "Initialization failed";

    isInitialized = true;

    return result;
}

int DsmrMonitorClient::awaitData()
{
    Tracer tracer("DsmrMonitorClient::requestData");

    int result = _httpClient.GET();
    TRACE("GET result: %d\n", result);

    if (result < 0)
    {
        _lastError = HTTPClient::errorToString(result);
        return result;
    }
    else if (result != HTTP_CODE_OK)
    {
        _lastError = "HTTP status code ";
        _lastError += result;
        return result;
    }
    else if (_httpClient.getSize() < MIN_CONTENT_LENGTH)
    {
        _lastError = "Unexpected Content Length: ";
        _lastError += _httpClient.getSize();
        return 0;
    }

    StreamString jsonResponse;
    jsonResponse.reserve(_httpClient.getSize());

    int bytesRead = _httpClient.writeToStream(&jsonResponse);
    if (bytesRead < 0)
    {
        _lastError = HTTPClient::errorToString(bytesRead);
        return bytesRead;
    }

    return parseJson(jsonResponse) ? HTTP_CODE_OK : 0;
}


bool DsmrMonitorClient::parseJson(String json)
{
    TRACE("JSON: '%s'\n", json.c_str());
    TRACE("\n");

    DeserializationError parseError = deserializeJson(_response, json);
    if (parseError != DeserializationError::Ok)
    {
        _lastError = "JSON error: "; 
        _lastError += parseError.c_str();
        return false;     
    }

    electricity.clear();
    JsonArray e = _response["Electricity"].as<JsonArray>();
    for (JsonVariant p : e)
    {
        PhaseData phaseData;
        phaseData.Name = p["Phase"].as<const char*>();
        phaseData.Voltage = p["U"];
        phaseData.Current = p["I"];
        phaseData.Power = p["Pdelivered"];
        phaseData.Power -= p["Preturned"].as<float>();
        electricity.push_back(phaseData);
    }

    TRACE("Deserialized %d electricity phases.\n", electricity.size());

    return true;
}
