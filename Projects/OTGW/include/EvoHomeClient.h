#ifndef EVOHOME_CLIENT_H
#define EVOHOME_CLIENT_H

#include <vector>
#include <RESTClient.h>

struct ZoneInfo
{
    float setpoint;
    float override;
    float actual;
    float heatDemand;
    float deviationHours;
};

class EvoHomeClient : public RESTClient
{
    public:
        std::vector<ZoneInfo> zones;

        EvoHomeClient(uint16_t timeout = 10, MemoryType memoryType = MemoryType::Internal)
            : RESTClient(timeout, memoryType) {}

        bool begin(const char* host);

    protected:
        virtual bool parseResponse(const JsonDocument& response) override;
};

#endif