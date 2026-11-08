#include "HomeWizardEnergySocket.h"

bool HomeWizardEnergySocketClient::begin(const char* host)
{
    String url = "http://";
    url += host;
    url += "/api/v1/data";

    return RESTClient::begin(url);
}

bool HomeWizardEnergySocketClient::parseResponse(const JsonDocument& response)
{
    wifiStrength = response["wifi_strength"].as<float>();
    totalPowerImportKwh = response["total_power_import_kwh"].as<float>();
    totalPowerExportKwh = response["total_power_export_kwh"].as<float>();
    activePowerW = response["active_power_w"].as<float>();
    activeVoltageV = response["active_voltage_v"].as<float>();
    activeCurrentA = response["active_current_a"].as<float>();
    activeReactivePowerVar = response["active_reactive_power_var"].as<float>();
    activeApparentPowerVa = response["active_apparent_power_va"].as<float>();
    activePowerFactor = response["active_power_factor"].as<float>();
    activeFrequencyHz = response["active_frequency_hz"].as<float>();

    return true;
}
