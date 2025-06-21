#include "HomeWizardP1Client.h"
#include <ArduinoJson.h>
#include <Tracer.h>

#ifdef ESP32
#include <HTTPClient.h>
#else
#include <ESP8266HTTPClient.h>
#endif


const char* _homeWizardCertificate = "-----BEGIN CERTIFICATE-----\n" \
"MIIDITCCAgkCFDn7cwYLioTM3VxdAygLl/Px9ovFMA0GCSqGSIb3DQEBCwUAME0x\n" \
"CzAJBgNVBAYTAk5MMQswCQYDVQQIDAJaSDETMBEGA1UECgwKSG9tZVdpemFyZDEc\n" \
"MBoGA1UEAwwTQXBwbGlhbmNlIEFjY2VzcyBDQTAeFw0yMTEyMTgxOTEyMTJaFw0z\n" \
"MTEyMTYxOTEyMTJaME0xCzAJBgNVBAYTAk5MMQswCQYDVQQIDAJaSDETMBEGA1UE\n" \
"CgwKSG9tZVdpemFyZDEcMBoGA1UEAwwTQXBwbGlhbmNlIEFjY2VzcyBDQTCCASIw\n" \
"DQYJKoZIhvcNAQEBBQADggEPADCCAQoCggEBAPBIvW8NRffqdvzHZY0M32fQHiGm\n" \
"pJgNGhiaQmpJfRDhT9yihM0S/hYcN8IqnfrMqoCQb/56Ub0+dZizmtfcGsE+Lpm1\n" \
"K1znkWqSDlpnuTNOb70TrsxBmbFuNOZQEi/xOjzT2j98wT0GSfxz1RVq6lZhDRRz\n" \
"xoe08+Xo4+ttUGanfOggJi0BXygeFEVBpbctVVJ9EgqeEE9itjcMlcxMe1QN14f8\n" \
"hCcOnId+9PSsdmyUCLrTB0FVYrbNfbJPk/vMU57fu6swBjWhYBxPx9ZhFy+7WnPR\n" \
"9BFg4seHNVQIqZNrf1YwBXlmZQIL32SRPaiH/+AVNMrYGXBvncY0Km6ZHIMCAwEA\n" \
"ATANBgkqhkiG9w0BAQsFAAOCAQEA6ybM8xm0PCXg8Rr/q0v1vPxQy44PmwXTDj0e\n" \
"r2vW4ZMiEwXZCp0Kk2K16KJYz4iJyfiQk8ikAIMiRSbyXzmyQ7XmL1O4l4d8E1Pg\n" \
"8EImvcyoBxFhd0Lq7VKriLc8Bw8SXbahPMGT+Y8Yz0uIsLAYVwlkLfgppVPmBaLD\n" \
"QautcQnI8WxPvCIQf5anyzgAyJC5ac6/CkB+iyPcuWcG3RMYvXnC0QoTlRa5YMlE\n" \
"FweVDlT2C/MdDyOxiAD/H1EP/eaySnU0zsxyD0yNFRKsQfQ+UJEPd2GS1AGA1lTy\n" \
"CGdyYj/Gghrusw0hM4rYXQSERWGF0mpEnuJ+7bHDolHu0rzgTQ==\n" \
"-----END CERTIFICATE-----\n";



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

    String baseUrl = "https://";
    baseUrl += host;
    baseUrl += "/api/";

    bool success = RESTClient::begin(baseUrl);
    addHeader("X-Api-Version", "2");
    addHeader("Content-Type", "application/json");
  
    return success;
}


String HomeWizardP1V2Client::getBearerToken(const String& name)
{
    Tracer tracer("HomeWizardP1V2Client::getBearerToken", name.c_str());

//#ifdef ESP32    
    char payloadJson[64];
    snprintf(payloadJson, sizeof(payloadJson), "{ \"name\": \"local/%s\" }", name.c_str());

    String response;
    if (request(RequestMethod::POST, "user", payloadJson, response) != HTTP_OK) return "";

    JsonDocument responseDoc;
    if (deserializeJson(responseDoc, response) != DeserializationError::Ok)
    {
        setLastError("Invalid response");
        return "";
    }

    return responseDoc["token"].as<String>();
//#else
//    return "";
//#endif
}


bool HomeWizardP1V2Client::setBatteryMode(bool enable)
{
    const char* newMode = enable ? "zero" : "standby";

    char payloadJson[64];
    snprintf(payloadJson, sizeof(payloadJson), "{ \"mode\": \"%s\" }", newMode);

    String response;
    if (request(RequestMethod::PUT, "batteries", payloadJson, response) != HTTP_OK) 
        return false;

    JsonDocument responseDoc;
    if (deserializeJson(responseDoc, response) != DeserializationError::Ok)
    {
        setLastError("Invalid response");
        return false;    
    }
    parseResponse(responseDoc);

    if (batteries.mode != newMode)
    {
        setLastError("Mode change failed");
        return false;
    }

    return true;
}


bool HomeWizardP1V2Client::parseResponse(const JsonDocument& response)
{
    JsonDocument& responseDoc = const_cast<JsonDocument&>(response);

    if (!responseDoc["mode"].isNull())
    {
        batteries.mode = responseDoc["mode"].as<String>();
        batteries.power = responseDoc["power_w"].as<int>();
        batteries.targetPower = responseDoc["target_power_w"].as<int>();
        batteries.maxConsumptionPower = responseDoc["max_consumption_w"].as<int>();
        batteries.maxProductionPower = responseDoc["max_production_w"].as<int>();

        TRACE("Battery mode: '%s', power: %d W, target: %d W, max consumption: %d W, max production: %d W\n",
              batteries.mode.c_str(), batteries.power, batteries.targetPower,
              batteries.maxConsumptionPower, batteries.maxProductionPower);
        return true;    
    }

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
