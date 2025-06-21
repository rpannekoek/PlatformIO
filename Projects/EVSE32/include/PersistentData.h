#include <DallasTemperature.h>
#include <PersistentDataBase.h>

constexpr size_t MAX_BT_DEVICES = 4;
constexpr float DEFAULT_CURRENT_SCALE = (3.3F / 4096) * (1800 / 50);  // CT=1:1800, R=50  

struct Settings : WiFiSettingsWithFTP
{
    int ftpSyncEntries;
    char p1Meter[32];
    int evsePhase;
    int currentLimit;
    int authorizeTimeout;
    int tempLimit;
    float tempSensorOffset;
    DeviceAddress tempSensorAddress;
    float currentScale;
    uint16_t currentZero; // Not used anymore
    uint16_t registeredBeaconCount;
    uuid128_t registeredBeacons[MAX_BT_DEVICES];
    float noCurrentThreshold;
    int solarPowerThreshold;
    int solarOnOffDelay;
    char p1BearerToken[36];

    static constexpr size_t ADDITIONAL_DATA_SIZE =
        sizeof(tempSensorAddress) +
        sizeof(currentScale) +
        sizeof(currentZero) +
        sizeof(registeredBeaconCount) +
        sizeof(registeredBeacons);

    Settings() : WiFiSettingsWithFTP("EVSE", ADDITIONAL_DATA_SIZE)
    {
        addIntegerField(ftpSyncEntries, "FTP Sync Entries", 0, 200);
        addStringField(p1Meter, sizeof(p1Meter), "P1 Meter");
        addStringField(p1BearerToken, sizeof(p1BearerToken), "P1 Auth Token");
        addIntegerField(evsePhase, "EVSE Phase", 1, 3, 3);
        addIntegerField(currentLimit, "Current Limit", 6, 25, 16);
        addTimeSpanField(authorizeTimeout, "Authorize Timeout", 0, 3600, 15 * 60);
        addIntegerField(tempLimit, "Temperature Limit", 40, 60, 50);
        addFloatField(tempSensorOffset, "Temperature Offset", 1, -5.0, 5.0);
        addFloatField(noCurrentThreshold, "No current threshold", 2, 0, 1, 0.5);
        addIntegerField(solarPowerThreshold, "Solar power threshold", 0, 1000, 100);
        addTimeSpanField(solarOnOffDelay, "Solar on/off delay", SECONDS_PER_MINUTE, SECONDS_PER_HOUR, SECONDS_PER_HOUR);
    }

    void initialize() override
    {
        WiFiSettingsWithFTP::initialize();
        memset(tempSensorAddress, 0, sizeof(DeviceAddress));
        currentScale = DEFAULT_CURRENT_SCALE; 
        registeredBeaconCount = 0;
        memset(registeredBeacons, 0, sizeof(registeredBeacons));
    }

    void validate() override
    {
        WiFiSettingsWithFTP::validate();
        registeredBeaconCount = std::min(registeredBeaconCount, (uint16_t)MAX_BT_DEVICES);
    }
};

Settings PersistentData;
