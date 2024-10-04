#include <PersistentDataBase.h>
#include "Constants.h"

struct Settings : public WiFiSettingsWithFTP
{
    int ftpSyncEntries;

    Settings() : WiFiSettingsWithFTP(PSTR("SolarMiles"))
    {
        addIntegerField(ftpSyncEntries, "FTP sync entries", 0, POWER_LOG_SIZE);
    }

    void initialize() override
    {
        WiFiSettingsWithFTP::initialize();
    }

    void validate() override
    {
        WiFiSettingsWithFTP::validate();
    }
};

Settings PersistentData;
