#ifndef HWP1_CLIENT_H
#define HWP1_CLIENT_H

#include <RESTClient.h>

struct PhaseData
{
    String Name;
    float Voltage;
    float Current;
    float Power;
};

struct BatteryInfo
{
    String mode;
    int power;
    int targetPower;
    int maxConsumptionPower;
    int maxProductionPower;

    bool isInitialized() const { return !mode.isEmpty(); }
};

class HomeWizardP1V1Client : public RESTClient
{
    public:
        std::vector<PhaseData> electricity;
        float gasM3;
        uint64_t gasTimestamp;

        // Constructor
        HomeWizardP1V1Client(uint16_t timeout = 5) : RESTClient(timeout) {}

        bool begin(const char* host);

    protected:
        virtual bool parseResponse(const JsonDocument& response) override;
};


class HomeWizardP1V2Client : public RESTClient
{
    public:
        std::vector<PhaseData> electricity;
        BatteryInfo batteries;

        // Constructor
        HomeWizardP1V2Client(uint16_t timeout = 5) : RESTClient(timeout) {}

        bool begin(const char* host);
        String getBearerToken(const String& name);
        bool setBatteryMode(bool enable);

        int requestData(const String& urlSuffix = "") override
        {
            if (urlSuffix.isEmpty())
                return RESTClient::requestData("measurement");
            else
                return RESTClient::requestData(urlSuffix);
        }

    protected:
        bool parseResponse(const JsonDocument& response) override;
};

#endif