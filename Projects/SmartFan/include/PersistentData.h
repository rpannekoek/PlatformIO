#include <PersistentDataBase.h>
#include "Constants.h"

struct Settings : public WiFiSettingsWithFTP
{
    int ftpSyncEntries;
    int maxLevel;
    int maxLevelDuration;
    int humidityThreshold;
    float tOffset;
    float dacScale;
    float adcScale;

    Settings() : WiFiSettingsWithFTP(PSTR("SmartFan"))
    {
        addIntegerField(ftpSyncEntries, "FTP sync entries", 0, FAN_LOG_SIZE, 50);
        addIntegerField(maxLevel, "Max level (%)", 1, 100, 50);
        addTimeSpanField(maxLevelDuration, "Max level duration", 0, SECONDS_PER_HOUR, 15*SECONDS_PER_MINUTE);
        addIntegerField(humidityThreshold, "Humidity theshold (%)", 50, 100, 75);
        addFloatField(tOffset, "T<sub>offset</sub>", 1, -10, 10);
        addFloatField(dacScale, "DAC scale", 2, 10, 30, 20.7);
        addFloatField(adcScale, "ADC scale", 2, 250, 300, 267);
    }
};

Settings PersistentData;
