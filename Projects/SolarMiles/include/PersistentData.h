#include <PersistentDataBase.h>
#include <SerialUtils.h>
#include <Tracer.h>

constexpr size_t MAX_REGISTERED_INVERTERS = 4;
constexpr size_t MAX_INVERTER_NAME_LENGTH = 16;
constexpr size_t MAX_DC_CHANNELS_PER_INVERTER = 4;
constexpr float POWER_EQUALS_MARGIN = 1.0F;
constexpr int POWER_LOG_SIZE = 200;
const char* DEFAULT_DTU_SERIAL = "199990100000";


struct PowerLogEntry
{
    time_t time;
    float power[MAX_REGISTERED_INVERTERS][MAX_DC_CHANNELS_PER_INVERTER];

    void reset(int numInverters)
    {
        TRACE("PowerLogEntry::reset(%d)\n", numInverters);

        time = 0;
        for (int i = 0; i < numInverters; i++)
            for (int ch = 0; ch < MAX_DC_CHANNELS_PER_INVERTER; ch++)
                power[i][ch] = 0;
    }

    void aggregate(int aggregations, int numInverters)
    {
        TRACE("PowerLogEntry::aggregate(%d, %d)\n", aggregations, numInverters);

        for (int i = 0; i < numInverters; i++)
            for (int ch = 0; ch < MAX_DC_CHANNELS_PER_INVERTER; ch++)
                power[i][ch] /= aggregations;
    }

    void update(int inverter, int channel, float power)
    {
        TRACE("PowerLogEntry::update(%d, %d, %0.1f)\n", inverter, channel, power);

        this->power[inverter][channel] += power;
    }

    bool equals(PowerLogEntry* other, int numInverters)
    {
        for (int i = 0; i < numInverters; i++)
            for (int ch = 0; ch < MAX_DC_CHANNELS_PER_INVERTER; ch++)
                if (abs(power[i][ch] - other->power[i][ch]) >= POWER_EQUALS_MARGIN)
                    return false;
        return true;
    }
};

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

    Settings() : WiFiSettingsWithFTP(
        PSTR("SolarMiles"),
        sizeof(registeredInvertersCount) + sizeof(registeredInverters))
    {
        addStringField(dtuSerial, sizeof(dtuSerial), "DTU serial#", DEFAULT_DTU_SERIAL);
        addIntegerField(dtuTxLevel, "DTU Tx Level", RF24_PA_MIN, RF24_PA_MAX, RF24_PA_MAX);
        addIntegerField(ftpSyncEntries, "FTP sync entries", 0, POWER_LOG_SIZE);
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
        {
            if (registeredInverters[i].serial == serial)
                return i;
        }
        return -1;
    }
};

Settings PersistentData;
