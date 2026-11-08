#include "HomeWizardDiscovery.h"
#include <ESPmDNS.h>
#include <Tracer.h>

bool HomeWizardDiscovery::begin(uint8_t apiVersion)
{
    Tracer tracer("HomeWizardDiscovery::begin");

    _devices.clear();
    _lastError = "";

    if (apiVersion != 1 && apiVersion != 2)
    {
        _lastError = "Invalid API version";
        return false;
    }

    const char* serviceType = apiVersion == 1 ? "hwenergy" : "homewizard";
    int resultCount = MDNS.queryService(serviceType, "tcp");
    for (int i = 0; i < resultCount; i++)
    {
        HomeWizardDevice device;
        device.type = MDNS.txt(i, "product_type");
        device.name = MDNS.txt(i, "product_name");

        device.host = MDNS.hostname(i);
        if (!device.host.isEmpty())
        {
            if (!device.host.endsWith(".local"))
                device.host += ".local";
        }
        else
        {
            IPAddress address = MDNS.address(i);
            if (address != IPAddress())
                device.host = address.toString();
        }

        TRACE(
            "Found '%s' device: '%s' @ %s\n",
            device.type.c_str(),
            device.name.c_str(),
            device.host.c_str());

        if (!device.host.isEmpty())
            _devices.push_back(device);
    }

    return true;
}

std::vector<HomeWizardDevice> HomeWizardDiscovery::getDevices(const String& type) const
{
    if (type.isEmpty())
        return _devices;

    std::vector<HomeWizardDevice> matchingDevices;
    for (const HomeWizardDevice& device : _devices)
    {
        if (device.type == type)
            matchingDevices.push_back(device);
    }

    return matchingDevices;
}
