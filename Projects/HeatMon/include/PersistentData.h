#include <PersistentDataBase.h>
#include <DallasTemperature.h>

struct Settings : WiFiSettingsWithFTP
{
    float tBufferMax;
    float tBufferMaxDelta;
    DeviceAddress tempSensorAddress[3];
    float tempSensorOffset[3];

    bool inline isBufferEnabled()
    {
        return tBufferMax != 0;
    }

    Settings()
        : WiFiSettingsWithFTP(PSTR("HeatMon"), sizeof(tempSensorAddress) + sizeof(tempSensorOffset))
    {
        addFloatField(tBufferMax, PSTR("T<sub>buffer, max</sub>"), 1, 0, 90, 0);
        addFloatField(tBufferMaxDelta, PSTR("T<sub>buffer, delta</sub>"), 1, 1, 10, 5);
    }

    void initialize() override
    {
        WiFiSettingsWithFTP::initialize();
        memset(tempSensorAddress, 0, sizeof(tempSensorAddress));
        memset(tempSensorOffset, 0, sizeof(tempSensorOffset));
    }

    void validate() override
    {
        for (int i = 0; i < 3; i++)
        {
            tempSensorOffset[i] = std::max(std::min(tempSensorOffset[i], 2.0F), -2.0F);
        }
    }
};

Settings PersistentData;
