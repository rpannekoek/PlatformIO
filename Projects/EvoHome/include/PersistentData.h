#include <PersistentDataBase.h>

struct Settings : public WiFiSettingsWithFTP
{
    int ftpSyncEntries;
    bool ftpSyncPacketLog;

    Settings() : WiFiSettingsWithFTP("EvoHome")
    {
        addIntegerField(ftpSyncEntries, "FTP sync entries", 0, RAMSES_PACKET_LOG_SIZE);
        addBooleanField(ftpSyncPacketLog, "FTP sync Packet Log", false, 4);
    }
};

Settings PersistentData;
