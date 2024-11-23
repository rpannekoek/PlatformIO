#include <PersistentDataBase.h>

struct Settings : public WiFiSettingsWithFTP
{
    int ftpSyncEntries;

    Settings() : WiFiSettingsWithFTP("EvoHome")
    {
        addIntegerField(ftpSyncEntries, "FTP sync entries", 0, RAMSES_PACKET_LOG_SIZE);
    }
};

Settings PersistentData;
