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
    char fritzbox[32];
    char fritzUser[32];
    char fritzPassword[32];

    Settings() : WiFiSettingsWithFTP(
        PSTR("SolarMiles"),
        sizeof(registeredInvertersCount) + sizeof(registeredInverters))
    {
        addStringField(dtuSerial, sizeof(dtuSerial), "DTU serial#", DEFAULT_DTU_SERIAL);
        addIntegerField(dtuTxLevel, "DTU Tx Level", RF24_PA_MIN, RF24_PA_MAX, RF24_PA_MAX);
        addIntegerField(ftpSyncEntries, "FTP sync entries", 0, POWER_LOG_SIZE);
        addStringField(fritzbox, sizeof(fritzbox), "Fritzbox");
        addStringField(fritzUser, sizeof(fritzUser), "Fritz user", "smarthomeuser");
        addPasswordField(fritzPassword, sizeof(fritzPassword), "Fritz password");
    }

    void initialize() override
    {
        WiFiSettingsWithFTP::initialize();
        registeredInvertersCount = 0;
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
