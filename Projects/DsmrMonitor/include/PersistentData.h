#include <PersistentDataBase.h>

struct PersistentSettings : WiFiSettingsWithFTP
{
    int ftpSyncEntries;
    int phaseCount; // 1 or 3
    int maxPhaseCurrent; // A (per phase)
    int powerLogDelta;
    float gasCalorificValue; // kWh per m3

    PersistentSettings() : WiFiSettingsWithFTP(PSTR("DsmrMonitor"))
    {
        addIntegerField(ftpSyncEntries, PSTR("FTP Sync entries"), 0, 250, 50);
        addIntegerField(phaseCount, PSTR("#Phases"), 1, 3, 1);
        addIntegerField(maxPhaseCurrent, PSTR("Max phase current"), 25, 75, 25);
        addIntegerField(powerLogDelta, PSTR("Power log delta (W)"), 0, 1000, 10);
        addFloatField(gasCalorificValue, PSTR("Gas calorific (kWh/m3)"), 3, 1, 15, 9.769);
    }
};

PersistentSettings PersistentData;
