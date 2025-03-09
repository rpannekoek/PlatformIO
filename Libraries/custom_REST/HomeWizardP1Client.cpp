#include "HomeWizardP1Client.h"
#include <ArduinoJson.h>
#include <Tracer.h>

#ifdef ESP32
#include <HTTPClient.h>
#else
#include <ESP8266HTTPClient.h>
#endif


const char* _homeWizardCertificate = "-----BEGIN CERTIFICATE-----" \
"MIIDITCCAgkCFDn7cwYLioTM3VxdAygLl/Px9ovFMA0GCSqGSIb3DQEBCwUAME0x" \
"CzAJBgNVBAYTAk5MMQswCQYDVQQIDAJaSDETMBEGA1UECgwKSG9tZVdpemFyZDEc" \
"MBoGA1UEAwwTQXBwbGlhbmNlIEFjY2VzcyBDQTAeFw0yMTEyMTgxOTEyMTJaFw0z" \
"MTEyMTYxOTEyMTJaME0xCzAJBgNVBAYTAk5MMQswCQYDVQQIDAJaSDETMBEGA1UE" \
"CgwKSG9tZVdpemFyZDEcMBoGA1UEAwwTQXBwbGlhbmNlIEFjY2VzcyBDQTCCASIw" \
"DQYJKoZIhvcNAQEBBQADggEPADCCAQoCggEBAPBIvW8NRffqdvzHZY0M32fQHiGm" \
"pJgNGhiaQmpJfRDhT9yihM0S/hYcN8IqnfrMqoCQb/56Ub0+dZizmtfcGsE+Lpm1" \
"K1znkWqSDlpnuTNOb70TrsxBmbFuNOZQEi/xOjzT2j98wT0GSfxz1RVq6lZhDRRz" \
"xoe08+Xo4+ttUGanfOggJi0BXygeFEVBpbctVVJ9EgqeEE9itjcMlcxMe1QN14f8" \
"hCcOnId+9PSsdmyUCLrTB0FVYrbNfbJPk/vMU57fu6swBjWhYBxPx9ZhFy+7WnPR" \
"9BFg4seHNVQIqZNrf1YwBXlmZQIL32SRPaiH/+AVNMrYGXBvncY0Km6ZHIMCAwEA" \
"ATANBgkqhkiG9w0BAQsFAAOCAQEA6ybM8xm0PCXg8Rr/q0v1vPxQy44PmwXTDj0e" \
"r2vW4ZMiEwXZCp0Kk2K16KJYz4iJyfiQk8ikAIMiRSbyXzmyQ7XmL1O4l4d8E1Pg" \
"8EImvcyoBxFhd0Lq7VKriLc8Bw8SXbahPMGT+Y8Yz0uIsLAYVwlkLfgppVPmBaLD" \
"QautcQnI8WxPvCIQf5anyzgAyJC5ac6/CkB+iyPcuWcG3RMYvXnC0QoTlRa5YMlE" \
"FweVDlT2C/MdDyOxiAD/H1EP/eaySnU0zsxyD0yNFRKsQfQ+UJEPd2GS1AGA1lTy" \
"CGdyYj/Gghrusw0hM4rYXQSERWGF0mpEnuJ+7bHDolHu0rzgTQ==" \
"-----END CERTIFICATE-----";


bool HomeWizardP1V1Client::begin(const char* host)
{
    Tracer tracer("HomeWizardP1V1Client::begin", host);

    String url = "http://";
    url += host;
    url += "/api/v1/data";

    return RESTClient::begin(url);
}

bool HomeWizardP1V1Client::parseResponse(const JsonDocument& response)
{
    JsonDocument& responseDoc = const_cast<JsonDocument&>(response);

    electricity.clear();

    char propertyName[32];
    for (int i = 1; i <= 3; i++)
    {
        String phaseName = "L";
        phaseName += i;

        snprintf(propertyName, sizeof(propertyName), "active_voltage_l%d_v", i);
        JsonVariant voltageJson = responseDoc[propertyName];
        if (voltageJson.isNull()) break;

        snprintf(propertyName, sizeof(propertyName), "active_current_l%d_a", i);
        JsonVariant currentJson = responseDoc[propertyName];

        snprintf(propertyName, sizeof(propertyName), "active_power_l%d_w", i);
        JsonVariant powerJson = responseDoc[propertyName];

        PhaseData phaseData
        {
            .Name = phaseName,
            .Voltage = voltageJson.as<float>(),
            .Current = currentJson.as<float>(),
            .Power = powerJson.as<float>()
        };
        electricity.push_back(phaseData);       
    }

    TRACE("Received data for %d phases\n", electricity.size());

    gasM3 = responseDoc["total_gas_m3"];
    gasTimestamp = responseDoc["gas_timestamp"];
    return true;
}


bool HomeWizardP1V2Client::begin(const char* host)
{
    Tracer tracer("HomeWizardP1V2Client::begin", host);

    String url = "https://";
    url += host;
    url += "/api/measurement";

    bool success = RESTClient::begin(url, _homeWizardCertificate);
    addHeader("X-Api-Version", "2");

    return success;
}


String HomeWizardP1V2Client::getBearerToken(const String& name)
{
    Tracer tracer("HomeWizardP1V2Client::getBearerToken", name.c_str());

#ifdef ESP32    
    String url = getBaseUrl();
    url += "/api/user";

    char payloadJson[64];
    snprintf(payloadJson, sizeof(payloadJson), "{ name: \"local/%s\" }", name.c_str());

    HTTPClient httpClient;
    httpClient.addHeader("X-Api-Version", "2");
    httpClient.addHeader("Content-Type", "application/json");
    httpClient.begin(url, _homeWizardCertificate);

    String result;
    int httpResult = httpClient.POST(payloadJson);
    if (httpResult == 200)
    {
        String responseJson = httpClient.getString();
        TRACE("POST response:\n%s\n", responseJson.c_str());

        JsonDocument responseDoc;
        deserializeJson(responseDoc, responseJson);
        result = responseDoc["token"].as<String>();
        TRACE("Bearer token: %s\n", result.c_str());
    }
    else
    {
        String errorMessage;
        if (httpResult < 0)
            errorMessage = HTTPClient::errorToString(httpResult);
        else
        {
            String responseJson = httpClient.getString();
            TRACE("POST response %d:\n%s\n", httpResult, responseJson.c_str());
    
            JsonDocument responseDoc;
            deserializeJson(responseDoc, responseJson);

            errorMessage = "HTTP ";
            errorMessage += httpResult;
            errorMessage += " ";
            errorMessage += responseDoc["error"].as<String>();
        }
        TRACE("%s\n", errorMessage.c_str());
        setLastError(errorMessage);
    }

    return result;
#else
    return "";
#endif
}


bool HomeWizardP1V2Client::parseResponse(const JsonDocument& response)
{
    JsonDocument& responseDoc = const_cast<JsonDocument&>(response);

    electricity.clear();

    char propertyName[32];
    for (int i = 1; i <= 3; i++)
    {
        String phaseName = "L";
        phaseName += i;

        snprintf(propertyName, sizeof(propertyName), "voltage_l%d_v", i);
        JsonVariant voltageJson = responseDoc[propertyName];
        if (voltageJson.isNull()) break;

        snprintf(propertyName, sizeof(propertyName), "current_l%d_a", i);
        JsonVariant currentJson = responseDoc[propertyName];

        snprintf(propertyName, sizeof(propertyName), "power_l%d_w", i);
        JsonVariant powerJson = responseDoc[propertyName];

        PhaseData phaseData
        {
            .Name = phaseName,
            .Voltage = voltageJson.as<float>(),
            .Current = currentJson.as<float>(),
            .Power = powerJson.as<float>()
        };
        electricity.push_back(phaseData);       
    }

    TRACE("Received data for %d phases\n", electricity.size());
    return true;
}
