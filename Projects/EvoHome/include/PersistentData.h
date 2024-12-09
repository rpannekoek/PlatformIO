#include <PersistentDataBase.h>

struct Settings : public WiFiSettingsWithFTP
{
    int ftpSyncEntries;
    bool ftpSyncPacketLog;
    int maxHeaderBitErrors;
    int maxManchesterBitErrors;

    Settings() : WiFiSettingsWithFTP("EvoHome")
    {
        addIntegerField(ftpSyncEntries, "FTP sync entries", 0, RAMSES_PACKET_LOG_SIZE);
        addBooleanField(ftpSyncPacketLog, "FTP sync Packet Log", false, 4);
        addIntegerField(maxHeaderBitErrors, "Max header bit errors", 0, 5, 0);
        addIntegerField(maxManchesterBitErrors, "Max manchester bit errors", 0, 10, 1);
    }
};

Settings PersistentData;
