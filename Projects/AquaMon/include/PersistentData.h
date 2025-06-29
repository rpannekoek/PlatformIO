#include <PersistentDataBase.h>

struct PersistentSettings : WiFiSettingsWithFTP
{
    int ftpSyncEntries;
    int antiFreezeTemp;
    bool logPacketErrors;
    float zone1Offset;
    char otgwHost[32];
    int solarPumpPWMDeltaT;

    PersistentSettings() : WiFiSettingsWithFTP(PSTR("AquaMon"))
    {
        addIntegerField(ftpSyncEntries, PSTR("FTP sync entries"), 1, 250, 50);
        addIntegerField(solarPumpPWMDeltaT, PSTR("Solar pump PWM ΔT"), 0, 50, 10);
        addIntegerField(antiFreezeTemp, PSTR("Anti-freeze temperature"), 1, 10, 5);
        addBooleanField(logPacketErrors, PSTR("Log packet errors"), false, 4); // 4 bytes for alignment
        addFloatField(zone1Offset, PSTR("Zone1 offset"), 1, -5.0F, 5.0F);
        addStringField(otgwHost, sizeof(otgwHost), PSTR("OTGW host"));
    }
};

PersistentSettings PersistentData;
