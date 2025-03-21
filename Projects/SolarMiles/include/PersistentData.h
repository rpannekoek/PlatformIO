#include <PersistentDataBase.h>
#include "Constants.h"
#include "SerialUtils.h"

struct RegisteredInverter
{
    uint64_t serial;
    char name[MAX_INVERTER_NAME_LENGTH];

    void copy(const RegisteredInverter& other)
    {
        serial = other.serial;
        strncpy(name, other.name, MAX_INVERTER_NAME_LENGTH);
    }
};

struct Settings : public WiFiSettingsWithFTP
{
    char dtuSerial[16];
    int dtuTxLevel;
    int ftpSyncEntries;
    size_t registeredInvertersCount;
    RegisteredInverter registeredInverters[MAX_REGISTERED_INVERTERS];
    bool enableFritzSmartHome;
    char smartThingsPAT[48];
    int powerThreshold;
    int idleDelay;
    char p1MonitorHost[32];
    uint8_t inverterPhase[MAX_REGISTERED_INVERTERS];

    Settings() : WiFiSettingsWithFTP(
        PSTR("SolarMiles"),
        sizeof(registeredInvertersCount) + sizeof(registeredInverters) + sizeof(inverterPhase))
    {
        addStringField(dtuSerial, sizeof(dtuSerial), "DTU serial#", DEFAULT_DTU_SERIAL);
        addIntegerField(dtuTxLevel, "DTU Tx Level", RF24_PA_MIN, RF24_PA_MAX, RF24_PA_MAX);
        addIntegerField(ftpSyncEntries, "FTP sync entries", 0, POWER_LOG_SIZE);
        addBooleanField(enableFritzSmartHome, "Fritz! SmartHome", false, 4);
        addStringField(smartThingsPAT, sizeof(smartThingsPAT), "SmartThings PAT");
        addIntegerField(powerThreshold, "Power threshold", 0, 100, 5);
        addTimeSpanField(idleDelay, "Idle delay", 0, 3600, 5 * 60);
        addStringField(p1MonitorHost, sizeof(p1MonitorHost), "P1 Monitor");
    }

    void initialize() override
    {
        WiFiSettingsWithFTP::initialize();
        registeredInvertersCount = 0;
        memset(inverterPhase, 0, sizeof(inverterPhase));
    }

    void validate() override
    {
        WiFiSettingsWithFTP::validate();
        registeredInvertersCount = std::min(registeredInvertersCount, MAX_REGISTERED_INVERTERS);
        if (parseSerial(dtuSerial) == 0)
            strcpy(dtuSerial, DEFAULT_DTU_SERIAL);
    }

    int getRegisteredInverter(uint64_t serial)
    {
        for (int i = 0; i < registeredInvertersCount; i++)
            if (registeredInverters[i].serial == serial)
                return i;
        return -1;
    }
};

Settings PersistentData;
