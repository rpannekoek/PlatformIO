#ifndef HOMEWIZARD_ENERGY_SOCKET_H
#define HOMEWIZARD_ENERGY_SOCKET_H

#include <RESTClient.h>

class HomeWizardEnergySocketClient : public RESTClient
{
    public:
        float wifiStrength = 0;
        float totalPowerImportKwh = 0;
        float totalPowerExportKwh = 0;
        float activePowerW = 0;
        float activeVoltageV = 0;
        float activeCurrentA = 0;
        float activeReactivePowerVar = 0;
        float activeApparentPowerVa = 0;
        float activePowerFactor = 0;
        float activeFrequencyHz = 0;
        
        HomeWizardEnergySocketClient(uint16_t timeout = 5) : RESTClient(timeout) {}

        bool begin(const char* host);

    protected:
        bool parseResponse(const JsonDocument& response) override;
};

#endif
