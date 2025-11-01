#include "OnectaClient.h"
#include <Tracer.h>
#include <TimeUtils.h>
#include <StreamUtils.h>

constexpr const char* IDP_TOKEN_URL = "https://idp.onecta.daikineurope.com/v1/oidc/token";
constexpr const char* API_BASE_URL = "https://api.onecta.daikineurope.com/v1/";
constexpr const char* RATE_LIMIT_PER_DAY_HEADER = "X-RateLimit-Limit-day";
constexpr const char* RATE_LIMIT_REMAINING_HEADER = "X-RateLimit-Remaining-day";
constexpr uint32_t MIN_REMAINING = 10;
constexpr uint32_t SKEW_TIME_MS = 10000;

OnectaClient::OnectaClient(const char* clientId, const char* clientSecret, char* refreshToken, ILogger& logger)
    :  jsonDoc(&_spiRamAllocator), _logger(logger)
{
    _clientId = clientId;
    _clientSecret = clientSecret;
    _refreshToken = refreshToken;

    _tlsClient.setInsecure(); // TODO: certificate

    _sitesFilterDoc[0]["gatewayDevices"] = true;

    _deviceFilterDoc["managementPoints"][0]["managementPointType"] = true;
    _deviceFilterDoc["managementPoints"][0]["name"]["value"] = true;
    _deviceFilterDoc["managementPoints"][0]["onOffMode"]["value"] = true;
    _deviceFilterDoc["managementPoints"][0]["operationMode"]["value"] = true;
    _deviceFilterDoc["managementPoints"][0]["sensoryData"]["value"]["roomTemperature"]["value"] = true;
    _deviceFilterDoc["managementPoints"][0]["sensoryData"]["value"]["outdoorTemperature"]["value"] = true;
    _deviceFilterDoc["managementPoints"][0]["consumptionData"]["value"]["electrical"]["heating"]["m"] = true;
    _deviceFilterDoc["managementPoints"][0]["consumptionData"]["value"]["electrical"]["cooling"]["m"] = true;
    _deviceFilterDoc["managementPoints"][0]["temperatureControl"]["value"]["operationModes"]["heating"]["setpoints"]["roomTemperature"]["value"] = true;
    _deviceFilterDoc["managementPoints"][0]["temperatureControl"]["value"]["operationModes"]["cooling"]["setpoints"]["roomTemperature"]["value"] = true;
    _deviceFilterDoc["managementPoints"][0]["temperatureControl"]["value"]["operationModes"]["auto"]["setpoints"]["roomTemperature"]["value"] = true;
}

bool OnectaClient::exchangeTokens()
{
    Tracer tracer("OnectaClient::exchangeTokens");

    String payload = String("grant_type=refresh_token");
    payload += String("&client_id=") + _clientId;
    payload += String("&client_secret=") + _clientSecret;
    payload += String("&refresh_token=") + _refreshToken;

    TRACE("%s\n", payload.substring(0, 250).c_str());

    HTTPClient httpClient;
    httpClient.useHTTP10(true); // Use HTTP 1.0 => prevent chunked encoding
    httpClient.begin(_tlsClient, IDP_TOKEN_URL);
    httpClient.addHeader("Content-Type", "application/x-www-form-urlencoded");
    int code = httpClient.POST(payload);
    if (code < 0)
    {
        _logger.logEvent("Onecta: %s", httpClient.errorToString(code).c_str());
        return false;
    }
    if (code != HTTP_CODE_OK)
    {
        _logger.logEvent("Onecta: HTTP %d", code);
        String body = httpClient.getString();
        TRACE("%s\n", body.substring(0, 250).c_str());
        return false;
    }
    TRACE("HTTP Response size: %d\n", httpClient.getSize());

    JsonDocument responseDoc;
    DeserializationError parseError = deserializeJson(
        responseDoc,
        httpClient.getStream());

    httpClient.end();

    if (parseError != DeserializationError::Ok)
    {
        _logger.logEvent("Onecta: JSON error %s", parseError.c_str());
        return false;
    }

    JsonVariant error = responseDoc["error"];
    if (!error.isNull())
    {
        _logger.logEvent("Onecta: %s", error.as<const char*>());
        return false;
    }

    JsonVariant accessToken = responseDoc["access_token"];
    if (accessToken.isNull())
    {
        _logger.logEvent("Onecta: No access_token");
        return false;
    }

    JsonVariant refreshToken = responseDoc["refresh_token"];
    if (refreshToken.isNull())
    {
        _logger.logEvent("Onecta: No refresh_token");
        return false;
    }

    JsonVariant expiresIn = responseDoc["expires_in"];
    if (expiresIn.isNull())
    {
        _logger.logEvent("Onecta: No expires_in");
        return false;
    }

    _accessToken = accessToken.as<String>();
    strcpy(_refreshToken, refreshToken.as<const char*>()) ;
    _tokenExpiresMillis = millis() 
        + expiresIn.as<uint32_t>() * 1000
        - SKEW_TIME_MS;

    if (_refreshTokenCallback) _refreshTokenCallback();

    return true;
}

bool OnectaClient::request(const String& urlPath, const JsonDocument& filterDoc)
{
    Tracer tracer("OnectaClient::request", urlPath.c_str());

    if (millis() >= _tokenExpiresMillis)
    {
        if (!exchangeTokens()) return false;
    }

    String url = API_BASE_URL;
    url += urlPath;

    HTTPClient httpClient;
    httpClient.useHTTP10(true); // Use HTTP 1.0 => prevent chunked encoding
    if (!httpClient.begin(_tlsClient, url))
    {
        _logger.logEvent("Onecta: HTTPClient.begin failed.");
        return false;
    }

    httpClient.setAuthorizationType("Bearer");
    httpClient.setAuthorization(_accessToken.c_str());
    httpClient.collectAllHeaders();

    uint32_t startMillis = millis();
    int httpCode = httpClient.GET();
    _responseTimeMs = millis() - startMillis;

    if (httpCode < 0)
    {
        _logger.logEvent("Onecta: %s", httpClient.errorToString(httpCode).c_str());
        return false;
    }

    TRACE(
        F("HTTP %d response after %u ms. Size: %d\n"),
        httpCode,
        _responseTimeMs,
        httpClient.getSize());

    if (httpCode != HTTP_CODE_OK)
    {
        _logger.logEvent("Onecta: HTTP %d", httpCode);
        return false;
    }

    DeserializationError jsonError = DeserializationError::EmptyInput;
    if (awaitDataAvailable(httpClient.getStream(), 100))
    {
        jsonError = deserializeJson(
            jsonDoc,
            httpClient.getStream(),
            DeserializationOption::Filter(filterDoc),
            DeserializationOption::NestingLimit(20));
    }

    if (jsonError != DeserializationError::Ok)
        _logger.logEvent("Onecta: JSON error %s", jsonError.c_str());
 
    String rateLimitPerDay = httpClient.header(RATE_LIMIT_PER_DAY_HEADER);
    String rateLimitRemaining = httpClient.header(RATE_LIMIT_REMAINING_HEADER);
    _rateLimitPerDay = rateLimitPerDay.toInt();
    if (_rateLimitPerDay == 0) _rateLimitPerDay = 200;
    _rateLimitRemaining = rateLimitRemaining.toInt();

    TRACE("%s: '%s'\n", RATE_LIMIT_PER_DAY_HEADER, rateLimitPerDay.c_str());
    TRACE("%s: '%s'\n", RATE_LIMIT_REMAINING_HEADER, rateLimitRemaining.c_str());

    httpClient.end();

    return (jsonError == DeserializationError::Ok);
}

bool OnectaClient::discoverDevices()
{
    if (!request("sites", _sitesFilterDoc)) return false;

    deviceIds.clear();
    for (const JsonVariant site : jsonDoc.as<JsonArray>())
    {
        JsonArray gatewayDevices = site["gatewayDevices"].as<JsonArray>();
        for (const JsonVariant dev : gatewayDevices)
            deviceIds.push_back(dev.as<String>());
    }

    cleanup();

    return true;
}

bool OnectaClient::requestDeviceStatus(const String& deviceId, time_t time)
{
    if (time < _requestAfter)
    {
        TRACE("Rate limit. Request after %s\n", formatTime("%H:%M:%S", _requestAfter));
        return true;
    }

    String urlPath = "gateway-devices/";
    urlPath += deviceId;

    if (!request(urlPath, _deviceFilterDoc)) return false;

    if (_rateLimitRemaining > MIN_REMAINING)
    {
        uint32_t rateLimitDelay = SECONDS_PER_DAY / (_rateLimitPerDay - MIN_REMAINING);
        _requestAfter = time + rateLimitDelay;
    }
    else
        _requestAfter = time + SECONDS_PER_HOUR;

    return true;
}

void OnectaClient::cleanup()
{
    jsonDoc.clear();
    jsonDoc.shrinkToFit();
}
