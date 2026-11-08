#ifndef HOMEWIZARD_DISCOVERY_H
#define HOMEWIZARD_DISCOVERY_H

#include <Arduino.h>
#include <vector>

struct HomeWizardDevice
{
    String host;
    String name;
    String type;
};

class HomeWizardDiscovery
{
    public:
        bool begin(uint8_t apiVersion = 1);
        std::vector<HomeWizardDevice> getDevices(const String& type = "") const;
        String getLastError() const { return _lastError; }

    private:
        std::vector<HomeWizardDevice> _devices;
        String _lastError;
};

#endif
