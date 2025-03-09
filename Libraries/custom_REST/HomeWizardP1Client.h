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

        // Constructor
        HomeWizardP1V2Client(uint16_t timeout = 5) : RESTClient(timeout) {}

        bool begin(const char* host);
        String getBearerToken(const String& name);

    protected:
        virtual bool parseResponse(const JsonDocument& response) override;
};

#endif