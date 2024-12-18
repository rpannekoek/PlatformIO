#ifndef HEATMON_CLIENT_H
#define HEATMON_CLIENT_H

#include <RESTClient.h>

class HeatMonClient : public RESTClient
{
    public:
        float tIn;
        float tOut;
        float tBuffer;
        float flowRate;
        float pIn;
        bool valve;

        // Constructor
        HeatMonClient(uint16_t timeout = 10) : RESTClient(timeout) {}

        bool begin(const char* host);

        void setOffline()
        {
            flowRate = 0;
            pIn = 0;
        }

        bool isHeatpumpOn() { return pIn > 0.5; }

    protected:
        virtual bool parseResponse(const JsonDocument& response) override;
};

#endif