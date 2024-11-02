#include <Arduino.h>
#include <ESPCoreDump.h>
#include <ESPWiFi.h>
#include <ESPWebServer.h>
#include <ESPFileSystem.h>
#include <Hoymiles.h>
#include <WiFiNTP.h>
#include <WiFiFTP.h>
#include <TimeUtils.h>
#include <Tracer.h>
#include <StringBuilder.h>
#include <LED.h>
#include <Log.h>
#include <WiFiStateMachine.h>
#include <HtmlWriter.h>
#include <Navigation.h>
#include "Constants.h"
#include "PersistentData.h"
#include "EnergyLog.h"
#include "PowerLog.h"
#include "SmartHome.h"

enum FileId
{
    Logo,
    Styles,
    HomeIcon,
    GraphIcon,
    LogFileIcon,
    SettingsIcon,
    UploadIcon,
    _LastFile
};

const char* Files[] =
{
    "Logo.png",
    "styles.css",
    "Home.svg",
    "Graph.svg",
    "LogFile.svg",
    "Settings.svg",
    "Upload.svg"
};

const char* Timeframes[] = 
{
    "today",
    "day",
    "week",
    "month"
};

ESPWebServer WebServer(80); // Default HTTP port
WiFiNTP TimeServer;
WiFiFTPClient FTPClient(FTP_TIMEOUT_MS);
StringBuilder HttpResponse(8 * 1024);
HtmlWriter Html(HttpResponse, Files[Logo], Files[Styles], MAX_BAR_LENGTH);
StringLog EventLog(MAX_EVENT_LOG_SIZE, 128);
SimpleLED BuiltinLED(LED_BUILTIN);
WiFiStateMachine WiFiSM(BuiltinLED, TimeServer, WebServer, EventLog);
Navigation Nav;
SPIClass NRFSPI;
SmartHomeClass SmartHome(WiFiSM);

StaticLog<PowerLogEntry> PowerLog(POWER_LOG_SIZE);
PowerLogEntry* lastPowerLogEntryPtr = nullptr;
PowerLogEntry newPowerLogEntry;
int powerLogAggregations = 0;
int powerLogEntriesToSync = 0;
bool ftpSyncEnergy = false;

EnergyLog TotalEnergyLog;
std::vector<InverterLog*> InverterLogPtrs;

uint32_t pollInterval = POLL_INTERVAL_DAY;
time_t currentTime = 0;
time_t pollInvertersTime = 0;
time_t syncFTPTime = 0;
time_t lastFTPSyncTime = 0;

// Forward defines
void onTimeServerSynced();
void onWiFiInitialized();
void handleSerialRequest();
void handleHttpRootRequest();
void handleHttpSyncFTPRequest();
void handleHttpPowerLogRequest();
void handleHttpEventLogRequest();
void handleHttpConfigFormRequest();
void handleHttpConfigFormPost();
void handleHttpInvertersFormRequest();
void handleHttpInvertersFormPost();
void handleHttpGridProfileRequest();
void handleHttpSmartHomeRequest();
bool trySyncFTP(Print* printTo);


bool addInverter(const char* name, uint64_t serial)
{
    Tracer tracer("addInverter", name);

    auto inverterPtr = Hoymiles.addInverter(name, serial);
    if (inverterPtr == nullptr)
    {
        WiFiSM.logEvent("Adding inverter '%s' (#%s) failed", name, formatSerial(serial));
        return false;
    }
    inverterPtr->setEnablePolling(true);
    inverterPtr->setZeroYieldDayOnMidnight(true);

    InverterLogPtrs.push_back(new InverterLog());
    PowerLog.clear();
    return true;
}


void removeInverter(int index)
{
    Tracer tracer("removeInverter", String(index).c_str());

    Hoymiles.removeInverterBySerial(PersistentData.registeredInverters[index].serial);

    if (index < --PersistentData.registeredInvertersCount)
    {
        for (int i = index; i < PersistentData.registeredInvertersCount; i++)
            PersistentData.registeredInverters[i].copy(PersistentData.registeredInverters[i + 1]);
    }

    delete InverterLogPtrs[index];
    InverterLogPtrs.erase(InverterLogPtrs.begin() + index);
    PowerLog.clear();
}

// Boot code
void setup() 
{
    Serial.begin(DEBUG_BAUDRATE);
    Serial.setDebugOutput(true);
    Serial.println();

    #ifdef DEBUG_ESP_PORT
    Tracer::traceTo(DEBUG_ESP_PORT);
    Tracer::traceFreeHeap();
    #endif

    BuiltinLED.begin();
    EventLog.begin();

    PersistentData.begin();
    TimeServer.begin(PersistentData.ntpServer);
    Html.setTitlePrefix(PersistentData.hostName);
    
    Nav.menuItems =
    {
        MenuItem
        {
            .icon = Files[HomeIcon],
            .label = PSTR("Home"),
            .handler = handleHttpRootRequest            
        },
        MenuItem
        {
            .icon = Files[GraphIcon],
            .label = PSTR("Power log"),
            .urlPath = PSTR("powerlog"),
            .handler = handleHttpPowerLogRequest            
        },
        MenuItem
        {
            .icon = Files[LogFileIcon],
            .label = PSTR("Event log"),
            .urlPath =PSTR("events"),
            .handler = handleHttpEventLogRequest
        },
        MenuItem
        {
            .icon = Files[UploadIcon],
            .label = PSTR("FTP Sync"),
            .urlPath = PSTR("sync"),
            .handler= handleHttpSyncFTPRequest
        },
        MenuItem
        {
            .icon = Files[HomeIcon],
            .label = PSTR("Smart Home"),
            .urlPath =PSTR("smartHome"),
            .handler = handleHttpSmartHomeRequest
        },
        MenuItem
        {
            .icon = Files[SettingsIcon],
            .label = PSTR("Inverters"),
            .urlPath =PSTR("inverters"),
            .handler = handleHttpInvertersFormRequest,
            .postHandler = handleHttpInvertersFormPost
        },
        MenuItem
        {
            .icon = Files[SettingsIcon],
            .label = PSTR("Settings"),
            .urlPath =PSTR("config"),
            .handler = handleHttpConfigFormRequest,
            .postHandler = handleHttpConfigFormPost
        },
    };
    Nav.registerHttpHandlers(WebServer);

    WebServer.on("/gridprofile",handleHttpGridProfileRequest);

    WiFiSM.registerStaticFiles(Files, _LastFile);
    WiFiSM.on(WiFiInitState::TimeServerSynced, onTimeServerSynced);
    WiFiSM.on(WiFiInitState::Initialized, onWiFiInitialized);
    WiFiSM.scanAccessPoints();
    WiFiSM.begin(PersistentData.wifiSSID, PersistentData.wifiKey, PersistentData.hostName);

    uint64_t dtuSerial = parseSerial(PersistentData.dtuSerial);
    rf24_pa_dbm_e paLevel = static_cast<rf24_pa_dbm_e>(PersistentData.dtuTxLevel);

    Hoymiles.init();
    Hoymiles.setPollInterval(pollInterval);

    NRFSPI.begin(NRF_SCK, NRF_MISO, NRF_MOSI, NRF_CS_PIN);
    Hoymiles.initNRF(&NRFSPI, NRF_CE_PIN, NRF_IRQ_PIN);
    HoymilesRadio_NRF* nrfRadioPtr = Hoymiles.getRadioNrf();
    if (nrfRadioPtr->isConnected())
    {
        nrfRadioPtr->setDtuSerial(dtuSerial);
        nrfRadioPtr->setPALevel(paLevel);
    }
    else
        WiFiSM.logEvent("ERROR: NRF Radio is not connected.");

    for (int i = 0; i < PersistentData.registeredInvertersCount; i++)
    {
        addInverter(
            PersistentData.registeredInverters[i].name,
            PersistentData.registeredInverters[i].serial);  
    }

    Tracer::traceFreeHeap();

    BuiltinLED.setOn(false);    
}


// Called repeatedly
void loop() 
{
    currentTime = WiFiSM.getCurrentTime();

    if (Serial.available())
        handleSerialRequest();

    // Let WiFi State Machine handle initialization and web requests
    // This also calls the onXXX methods below
    WiFiSM.run();
}

void handleSerialRequest()
{
    Tracer tracer("handleSerialRequest");
    Serial.setTimeout(100);

    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    Serial.println(cmd);

    if (cmd.startsWith("testF"))
    {
        for (int i = 0; i < POWER_LOG_SIZE; i++)
        {
            newPowerLogEntry.time = currentTime + i * SECONDS_PER_MINUTE;
            newPowerLogEntry.dcPower[0][0] = i;
            newPowerLogEntry.dcPower[0][1] = (POWER_LOG_SIZE - i);
            lastPowerLogEntryPtr = PowerLog.add(&newPowerLogEntry);
        }
        powerLogEntriesToSync = 10;

        InverterLog* inverterLogPtr = InverterLogPtrs[0];
        inverterLogPtr->acEnergyLogPtr->init(currentTime);
        inverterLogPtr->dcEnergyLogPtrs.push_back(new EnergyLog(currentTime));
        inverterLogPtr->dcEnergyLogPtrs.push_back(new EnergyLog(currentTime));
        for (int i = 0; i < 20; i++)
        {
            time_t time = currentTime + i * TODAY_LOG_INTERVAL;
            int power1 = i * 10;
            int power2 = (20 - i) * 20;
            TotalEnergyLog.update(time, (power1 + power2));
            inverterLogPtr->acEnergyLogPtr->update(time, power1);
            if (i < 18)
                inverterLogPtr->dcEnergyLogPtrs[0]->update(time, power1);
            if (i > 1)
                inverterLogPtr->dcEnergyLogPtrs[1]->update(time, power2);
        }
    }
    else if (cmd.startsWith("ftp"))
    {
        SmartDeviceEnergyLogEntry testLogEntry;
        testLogEntry.devicePtr = SmartHome.devices[0];
        testLogEntry.start = currentTime - SECONDS_PER_HOUR;
        testLogEntry.end = currentTime;
        testLogEntry.maxPower = 666.1;
        testLogEntry.energyDelta = 6.666;
        SmartHome.energyLog.add(&testLogEntry);
        SmartHome.logEntriesToSync = 1;
        syncFTPTime = currentTime;
    }
    else if (cmd.startsWith("wifi"))
    {
        WiFiSM.traceDiag();
        WiFiSM.forceReconnect();
    }
    else if (cmd.startsWith("evt"))
    {
        for (int i = 0; i < MAX_EVENT_LOG_SIZE; i++)
            WiFiSM.logEvent("Test event to fill event log");
    }
}


void updatePowerLog(int numInverters)
{
    TRACE("updatePowerLog(%d)\n", numInverters);

    newPowerLogEntry.aggregate(powerLogAggregations, numInverters);
    if ((lastPowerLogEntryPtr == nullptr) || !newPowerLogEntry.equals(lastPowerLogEntryPtr, numInverters))
    {
        newPowerLogEntry.time = currentTime;
        lastPowerLogEntryPtr = PowerLog.add(&newPowerLogEntry);
        powerLogEntriesToSync = std::min(powerLogEntriesToSync + 1, POWER_LOG_SIZE);
        if (PersistentData.isFTPEnabled() && (powerLogEntriesToSync == PersistentData.ftpSyncEntries))
            syncFTPTime = currentTime;
    }
    newPowerLogEntry.reset(numInverters);
}


bool pollInverters()
{
    Tracer tracer("pollInverters");

    int numInverters = Hoymiles.getNumInverters();
    bool isAnyInverterReachable = false;
    float totalPower = 0;
    for (int i = 0; i < numInverters; i++)
    {
        auto inverterPtr = Hoymiles.getInverterByPos(i);
        if (inverterPtr == nullptr)
        {
            TRACE("\tHoymiles.getInverterByPos(%d) returned null.\n", i);
            continue;
        }
        StatisticsParser* inverterStatsPtr = inverterPtr->Statistics();
        InverterLog* inverterLogPtr = InverterLogPtrs[i];
        TRACE("Inverter '%s':\n", inverterPtr->name());

        if (inverterPtr->isReachable())
        {
            inverterLogPtr->isReachable = true;
            isAnyInverterReachable = true;                
        }
        else
        {
            if (inverterLogPtr->isReachable)
            {
                inverterLogPtr->isReachable = false;
                WiFiSM.logEvent("%s is not reachable", inverterPtr->name());
            }
            continue;
        }

        AlarmLogParser* inverterEventLogPtr = inverterPtr->EventLog();
        TRACE(
            "\t%d events in alarm log. Last processed event: %s\n",
            inverterEventLogPtr->getEntryCount(),
            formatTime("%F %H:%M:%S", inverterLogPtr->lastEventTime));
        time_t startOfDay = getStartOfDay(currentTime);
        time_t lastEventTime = 0;
        for (int i = 0; i < inverterEventLogPtr->getEntryCount(); i++)
        {
            AlarmLogEntry_t logEntry;
            inverterEventLogPtr->getLogEntry(i, logEntry);
            time_t startTime = logEntry.StartTime + startOfDay;
            TRACE(F("\t%s - %s\n"), formatTime("%F %H:%M:%S", startTime), logEntry.Message.c_str());
            if ((startTime > inverterLogPtr->lastEventTime) && (startTime <= currentTime))
            {
                WiFiSM.logEvent(
                    "%s @ %s: %s",
                    inverterPtr->name(),
                    formatTime("%H:%M", startTime),
                    logEntry.Message.c_str());
                lastEventTime = std::max(lastEventTime, startTime);
            }
        }
        if (lastEventTime != 0)
            inverterLogPtr->lastEventTime = lastEventTime;

        if (!inverterPtr->isProducing())
        {
            TRACE("\tNot producing.\n");
            continue;
        }

        for (ChannelNum_t channel : inverterStatsPtr->getChannelsByType(TYPE_DC))
        {
            float stringPower = inverterStatsPtr->getChannelFieldValue(TYPE_DC, channel, FLD_PDC);
            TRACE("\tDC channel #%d: %0.1f W\n", channel, stringPower);
            newPowerLogEntry.updateDC(i, channel, stringPower);
            if (inverterLogPtr->dcEnergyLogPtrs.size() <= channel)
            {
                for (int k = inverterLogPtr->dcEnergyLogPtrs.size(); k == channel; k++)
                    inverterLogPtr->dcEnergyLogPtrs.push_back(new EnergyLog(currentTime));
            }
            inverterLogPtr->dcEnergyLogPtrs[channel]->update(currentTime, stringPower);
        }

        float inverterPower = 0;
        float acVoltage = 0;
        std::list<ChannelNum_t> acChannels = inverterStatsPtr->getChannelsByType(TYPE_AC);
        for (ChannelNum_t channel : acChannels)
        {
            float phasePower = inverterStatsPtr->getChannelFieldValue(TYPE_AC, channel, FLD_PAC);
            float phaseVoltage = inverterStatsPtr->getChannelFieldValue(TYPE_AC, channel, FLD_UAC); 
            TRACE("\tAC phase #%d: %0.1f W %0.1f V\n", channel, phasePower, phaseVoltage);
            inverterPower += phasePower;
            totalPower += phasePower;
            acVoltage += phaseVoltage;
        }
        if (acChannels.size() > 0)
            acVoltage /= acChannels.size();
        newPowerLogEntry.updateAC(i, inverterPower, acVoltage);
        inverterLogPtr->acEnergyLogPtr->update(currentTime, inverterPower);
    }

    TRACE("Total power: %0.1f W\n", totalPower);
    bool isNewDay = (totalPower == 0) ? false : TotalEnergyLog.update(currentTime, totalPower);
    if (isNewDay && PersistentData.isFTPEnabled())
    {
        syncFTPTime = currentTime;
        ftpSyncEnergy = true;
    }

    if (isAnyInverterReachable)
    {
        if (++powerLogAggregations == POWER_LOG_AGGREGATIONS)
        {
            updatePowerLog(numInverters);
            powerLogAggregations = 0;
        }
    }
    else if (powerLogAggregations != 0)
    {
        newPowerLogEntry.reset(numInverters);
        powerLogAggregations = 0;
    }

    return isAnyInverterReachable;
}


void writePowerLogEntriesCsv(Print& output)
{
    std::vector<size_t> dcChannels;
    for (int i = 0; i < Hoymiles.getNumInverters(); i++)
    {
        auto inverterPtr = Hoymiles.getInverterByPos(i);
        if (inverterPtr == nullptr) continue;
        size_t dcChannelCount = inverterPtr->Statistics()->getChannelsByType(TYPE_DC).size();
        dcChannels.push_back(dcChannelCount);
    }

    PowerLogEntry* powerLogEntryPtr = PowerLog.getEntryFromEnd(powerLogEntriesToSync);
    while (powerLogEntryPtr != nullptr)
    {
        output.print(formatTime("%F %H:%M", powerLogEntryPtr->time));
        for (int i = 0; i < dcChannels.size(); i++)
        {
            int dcChannelCount = dcChannels[i];
            for (int ch = 0; ch < dcChannelCount; ch++)
                output.printf(";%0.1f", powerLogEntryPtr->dcPower[i][ch]);
            output.printf(";%0.1f", powerLogEntryPtr->acPower[i]);
            output.printf(";%0.1f", powerLogEntryPtr->acVoltage[i]);
        }
        output.println();

        powerLogEntryPtr = PowerLog.getNextEntry();
    }
}


bool trySyncFTP(Print* printTo)
{
    Tracer tracer("trySyncFTP");

    if (!FTPClient.begin(
        PersistentData.ftpServer,
        PersistentData.ftpUser,
        PersistentData.ftpPassword,
        FTP_DEFAULT_CONTROL_PORT,
        printTo))
    {
        FTPClient.end();
        return false;
    }

    char filename[64];
    snprintf(filename, sizeof(filename), "%s_Power.csv", PersistentData.hostName);
    bool success = false;
    WiFiClient& dataClient = FTPClient.append(filename);
    if (dataClient.connected())
    {
        if (powerLogEntriesToSync != 0)
        {
            writePowerLogEntriesCsv(dataClient);
            powerLogEntriesToSync = 0;
        }
        else if (printTo != nullptr)
            printTo->println("Nothing to sync.");
        dataClient.stop();

        if (FTPClient.readServerResponse() == 226)
        {
            lastFTPSyncTime = currentTime;
            success = true;
        }
        else
            FTPClient.setUnexpectedResponse();
    }

    if (success && ftpSyncEnergy)
    {
        EnergyLogEntry* energyLogEntryPtr = TotalEnergyLog.getYesterdayLogEntry();
        if (energyLogEntryPtr != nullptr)
        {
            if (FTPClient.passive())
            {
                snprintf(filename, sizeof(filename), "%s_TotalEnergy.csv", PersistentData.hostName);
                WiFiClient& dataClient = FTPClient.append(filename);
                if (dataClient.connected())
                {
                    dataClient.printf(
                        "\"%s\",%0.1f,%0.0f,%0.2f\r\n",
                        formatTime("%F", energyLogEntryPtr->time),
                        float(energyLogEntryPtr->onDuration) / SECONDS_PER_HOUR,
                        energyLogEntryPtr->maxPower,
                        energyLogEntryPtr->energy / 1000
                        );
                    dataClient.stop();
                }
                if (FTPClient.readServerResponse() == 226)
                {
                    lastFTPSyncTime = currentTime;
                    ftpSyncEnergy = false;
                }
                else
                {
                    FTPClient.setUnexpectedResponse();
                    success = false;                
                }
            }
            else
                success = false;
        }
    }

    if (success && SmartHome.logEntriesToSync != 0)
    {
        if (FTPClient.passive())
        {
            snprintf(filename, sizeof(filename), "%s_SmartHome.csv", PersistentData.hostName);
            WiFiClient& dataClient = FTPClient.append(filename);
            if (dataClient.connected())
            {
                SmartHome.writeEnergyLogCsv(dataClient);
                dataClient.stop();
            }
            if (FTPClient.readServerResponse() == 226)
            {
                lastFTPSyncTime = currentTime;
                SmartHome.logEntriesToSync = 0;
            }
            else
            {
                FTPClient.setUnexpectedResponse();
                success = false;                
            }
        }
        else
            success = false;
    }

    FTPClient.end();
    return success;
}


void onTimeServerSynced()
{
    currentTime = TimeServer.getCurrentTime();
    pollInvertersTime = currentTime;

    newPowerLogEntry.reset(Hoymiles.getNumInverters());

    TotalEnergyLog.init(currentTime);
    for (InverterLog* inverterLogPtr : InverterLogPtrs)
        inverterLogPtr->acEnergyLogPtr->init(currentTime);

    if (!WiFiSM.isInAccessPointMode())
    {
        if (PersistentData.enableFritzSmartHome && PersistentData.isFTPEnabled())
            SmartHome.useFritzbox(PersistentData.ftpServer, PersistentData.ftpUser, PersistentData.ftpPassword);

        if (PersistentData.smartThingsPAT[0] != 0)
            SmartHome.useSmartThings(PersistentData.smartThingsPAT);

        if (!SmartHome.begin(PersistentData.powerThreshold, PersistentData.idleDelay, 10))
            WiFiSM.logEvent("Unable to initialize SmartHome");
    }
}


void onWiFiInitialized()
{
    SmartHome.run();

    // Run the Hoymiles loop only after we obtained NTP time,
    // because an (Epoch) timestamp is included in the messages.
    Hoymiles.loop();

    if (currentTime >= pollInvertersTime)
    {
        BuiltinLED.setOn(true);
        if (pollInverters())
            pollInterval = POLL_INTERVAL_DAY;
        else if (pollInterval < POLL_INTERVAL_NIGHT)
        {
            pollInterval = std::min(pollInterval * 2, POLL_INTERVAL_NIGHT);
            if ((pollInterval == POLL_INTERVAL_NIGHT) && (PersistentData.ftpSyncEntries != 0) && PersistentData.isFTPEnabled())
                syncFTPTime = currentTime; // Force FTP sync at end of day 
        }
        Hoymiles.setPollInterval(pollInterval);
        pollInvertersTime = currentTime + pollInterval;
        delay(100); // Ensure LED blink is visible
        BuiltinLED.setOn(false);
    }

    if (SmartHome.logEntriesToSync != 0 && syncFTPTime == 0)
        syncFTPTime = currentTime + FTP_RETRY_INTERVAL; // Prevent FTP sync shortly after eachother

    if ((syncFTPTime != 0) && (currentTime >= syncFTPTime) && WiFiSM.isConnected())
    {
        if (trySyncFTP(nullptr))
        {
            WiFiSM.logEvent("FTP sync");
            syncFTPTime = 0;
        }
        else
        {
            WiFiSM.logEvent("FTP sync failed: %s", FTPClient.getLastError());
            syncFTPTime += FTP_RETRY_INTERVAL;
        }
    }
}


void writeTotalCard(const String& header, int value, const char* unit)
{
    Html.writeDivStart("total-card");
    Html.writeDivStart("card-header");
    HttpResponse.print(header);
    Html.writeDivEnd();
    Html.writeDivStart("card-value");
    HttpResponse.printf(F("%d %s"), value, unit);
    Html.writeDivEnd();
    Html.writeDivEnd();
}


void writeTotals()
{
    float acPower = 0;
    float acEnergyToday = 0;
    float dcEnergyTotal = 0;

    for (int i = 0; i < Hoymiles.getNumInverters(); i++)
    {
        auto inverterPtr = Hoymiles.getInverterByPos(i);
        if (inverterPtr == nullptr) continue;
        StatisticsParser* inverterStatsPtr = inverterPtr->Statistics();
        for (ChannelNum_t channel : inverterStatsPtr->getChannelsByType(TYPE_AC))
            acPower += inverterStatsPtr->getChannelFieldValue(TYPE_AC, channel, FLD_PAC);
        for (ChannelNum_t channel : inverterStatsPtr->getChannelsByType(TYPE_DC))
            dcEnergyTotal += inverterStatsPtr->getChannelFieldValue(TYPE_DC, channel, FLD_YT);
    }

    for (InverterLog* inverterLogPtr : InverterLogPtrs)
        acEnergyToday += inverterLogPtr->acEnergyLogPtr->getTotalEnergy(EnergyLogType::Today);

    Html.writeSectionStart("Totals");
    Html.writeDivStart("totals");
    writeTotalCard("AC Power", std::round(acPower), "W");
    writeTotalCard("AC Energy Today", std::round(acEnergyToday), "Wh");
    writeTotalCard("DC Energy Total", std::round(dcEnergyTotal), "kWh");
    Html.writeDivEnd();
    Html.writeSectionEnd();
}


void writeInverterDCRow(int inverter, StatisticsParser* inverterStatsPtr, ChannelNum_t channel)
{
    Html.writeRowStart("dc");
    HttpResponse.printf(
        F("<td><a href='/?channel=%d&inverter=%d'>DC #%d</a></td>"),
        TYPE_DC, inverter, channel + 1);
    Html.writeCell("%0.1f V", inverterStatsPtr->getChannelFieldValue(TYPE_DC, channel, FLD_UDC)); 
    Html.writeCell("%0.2f A", inverterStatsPtr->getChannelFieldValue(TYPE_DC, channel, FLD_IDC)); 
    Html.writeCell("%0.1f W", inverterStatsPtr->getChannelFieldValue(TYPE_DC, channel, FLD_PDC)); 
    Html.writeCell("%0.1f Wh", inverterStatsPtr->getChannelFieldValue(TYPE_DC, channel, FLD_YD)); 
    Html.writeCell("%0.1f kWh", inverterStatsPtr->getChannelFieldValue(TYPE_DC, channel, FLD_YT)); 
    Html.writeRowEnd();
}

void writeInverterACRow(int inverter, StatisticsParser* inverterStatsPtr, ChannelNum_t channel)
{
    Html.writeRowStart("ac");
    HttpResponse.printf(
        F("<td><a href='/?channel=%d&inverter=%d'>AC</a></td>"),
        TYPE_AC, inverter);
    Html.writeCell("%0.1f V", inverterStatsPtr->getChannelFieldValue(TYPE_AC, channel, FLD_UAC)); 
    Html.writeCell("%0.2f A", inverterStatsPtr->getChannelFieldValue(TYPE_AC, channel, FLD_IAC)); 
    Html.writeCell("%0.1f W", inverterStatsPtr->getChannelFieldValue(TYPE_AC, channel, FLD_PAC)); 
    Html.writeCell("%0.1f VAr", inverterStatsPtr->getChannelFieldValue(TYPE_AC, channel, FLD_Q));
    Html.writeCell("%0.2f Hz", inverterStatsPtr->getChannelFieldValue(TYPE_AC, channel, FLD_F));
    Html.writeRowEnd();
}


void writeInverterData()
{
    Html.writeSectionStart("Inverter data");
    Html.writeTableStart();

    for (int i = 0; i < Hoymiles.getNumInverters(); i++)
    {
        auto inverterPtr = Hoymiles.getInverterByPos(i);
        if (inverterPtr == nullptr)
        {
            TRACE("Hoymiles.getInverterByPos(%d) returned null.\n", i);
            continue;
        }
 
        const char* status;
        if (inverterPtr->isReachable())
            status = inverterPtr->isProducing() ? "producing" : "online"; 
        else
            status = "offline";
        uint32_t dataAge = (millis() - inverterPtr->Statistics()->getLastUpdate()) / 1000; // TODO: millis() rollover
        time_t lastUpdateTime = currentTime - dataAge;
        StatisticsParser* inverterStatsPtr = inverterPtr->Statistics();

        // Inverter header row
        HttpResponse.printf(F("<tr class='%s'><th colspan='6'><div class='inverter'>"), status, inverterPtr->name());
        HttpResponse.printf(F("<div class='inv-name'>%s</div>"), inverterPtr->name());
        HttpResponse.printf(F("<div class='inv-updated'>%s</div>"), formatTime("%H:%M:%S", lastUpdateTime));
        HttpResponse.printf(
            F("<div><div class='inv-temp'>%0.1f °C</div><div class='inv-eff'>%0.1f %%</div></div>"),
            inverterStatsPtr->getChannelFieldValue(TYPE_INV, CH0, FLD_T),
            inverterStatsPtr->getChannelFieldValue(TYPE_INV, CH0, FLD_EFF));
        HttpResponse.print("</div></th></tr>");

        for (ChannelNum_t channel : inverterStatsPtr->getChannelsByType(TYPE_DC))
            writeInverterDCRow(i, inverterStatsPtr, channel);
        for (ChannelNum_t channel : inverterStatsPtr->getChannelsByType(TYPE_AC))
            writeInverterACRow(i, inverterStatsPtr, channel);
    }

    Html.writeTableEnd();
    Html.writeSectionEnd();
}


void writeGraphRow(
    std::vector<EnergyLogEntry*> energyLogEntryPtrs,
    time_t time,
    const char* timeFormat,
    int energyDivisor,
    float maxValue)
{
    String onDurationHtml;
    String maxPowerHtml;
    String energyHtml;
    char buffer[256];
    for (EnergyLogEntry* logEntryPtr : energyLogEntryPtrs)
    {
        uint32_t onDuration = 0;
        float maxPower = 0;
        float energy = 0;
        if (logEntryPtr == nullptr)
            TRACE("null, ");
        else
            TRACE(F("%c%s, "), (logEntryPtr->time == time) ? '+' : '-', formatTime("%F %H:%M:%S", logEntryPtr->time));
        if ((logEntryPtr != nullptr) && (logEntryPtr->time == time))
        {
            onDuration = logEntryPtr->onDuration;
            maxPower = logEntryPtr->maxPower;
            energy = logEntryPtr->energy / energyDivisor;
        }
        snprintf(buffer, sizeof(buffer), "<div>%s</div>", formatTimeSpan(onDuration));
        onDurationHtml += buffer;
        snprintf(buffer, sizeof(buffer), "<div>%0.0f</div>", maxPower);
        maxPowerHtml += buffer;
        snprintf(buffer, sizeof(buffer), "<div>%0.2f</div>", energy);
        energyHtml += buffer;
    }
    TRACE("\n");

    Html.writeRowStart();
    Html.writeCell(formatTime(timeFormat, time));
    Html.writeCell(onDurationHtml);
    Html.writeCell(maxPowerHtml);
    Html.writeCell(energyHtml);

    Html.writeCellStart("graph");
    for (EnergyLogEntry* logEntryPtr : energyLogEntryPtrs)
    {
        float barValue = 0.0F;
        if (logEntryPtr != nullptr && logEntryPtr->time == time)
            barValue = logEntryPtr->energy / maxValue;

        Html.writeBar(barValue, "energyBar", false, true, MAX_BAR_LENGTH);
    }
    Html.writeCellEnd();

    Html.writeRowEnd();
}


void writeEnergyLogs(int inverter, ChannelType_t channel,  EnergyLogType logType)
{
    TRACE(F("writeEnergyLogs(%d, %d, %d)\n"), inverter, channel, logType);

    String caption;
    std::vector<EnergyLog*> energyLogPtrs;
    if ((inverter >= 0) && (inverter < Hoymiles.getNumInverters()))
    {
        if (channel == TYPE_DC)
        {
            caption = "DC energy for inverter: ";
            caption += Hoymiles.getInverterByPos(inverter)->name();
            energyLogPtrs = InverterLogPtrs[inverter]->dcEnergyLogPtrs;
        }
        else
        {
            caption = "AC energy for all inverters";
            for (InverterLog* inverterLogPtr : InverterLogPtrs)
                energyLogPtrs.push_back(inverterLogPtr->acEnergyLogPtr);
        }
    }
    else
    {
        caption = "Total AC energy";
        energyLogPtrs.push_back(&TotalEnergyLog);
    }
    TRACE(F("Show %d Energy Logs\n"), energyLogPtrs.size());

    String queryString = "channel=";
    queryString += channel;
    if (inverter >= 0)
    {
        queryString += "&inverter=";
        queryString += inverter;
    }

    String timeframe = Timeframes[logType];
    const char* timeFormat;
    const char* unitOfMeasure = "kWh";
    int energyDivisor = 1000;
    switch (logType)
    {
        case EnergyLogType::PerDay:
            timeFormat = "%a";
            break;

        case EnergyLogType::PerWeek:
            timeFormat = "%d %b";
            break;

        case EnergyLogType::PerMonth:
            timeFormat = "%b";
            break;

        default: // Today
            timeFormat = "%H:%M";
            unitOfMeasure = "Wh";
            energyDivisor = 1;
            break;
    }

    // Auto-ranging: determine max value from the log entries
    float maxValue = 0.1F; // Prevent division by zero
    for (EnergyLog* energyLogPtr : energyLogPtrs)
        maxValue = std::max(maxValue, energyLogPtr->getMaxEnergy(logType));
    TRACE(F("maxValue: %0.1f\n"), maxValue);

    HttpResponse.println("<section>");
    HttpResponse.printf(
        F("<h1>Energy per <span class=\"dropdown\"><span>%s</span>"),
        timeframe.c_str());
    Html.writeDivStart("dropdown-list");
    for (int i = EnergyLogType::Today; i <= EnergyLogType::PerMonth; i++)
    {
        if (i == logType) continue;
        HttpResponse.printf(
            F("<a href=\"?%s=%d&%s\">%s</a>\r\n"),
            TIMEFRAME_PARAM,
            i,
            queryString.c_str(),
            Timeframes[i]);
    }
    Html.writeDivEnd();
    HttpResponse.println("</span></h1>");

    Html.writeTableStart();
    HttpResponse.printf(F("<caption>%s</caption>"), caption.c_str());

    Html.writeRowStart();
    if (logType == EnergyLogType::Today)
        Html.writeHeaderCell("Time");
    else
        Html.writeHeaderCell(timeframe);
    Html.writeHeaderCell("On time");
    Html.writeHeaderCell("P<sub>max</sub> (W)");
    HttpResponse.printf(F("<th>E (%s)</th>"), unitOfMeasure);
    Html.writeRowEnd();

    bool moreEntries = false;
    std::vector<EnergyLogEntry*> logEntryPtrs;
    for (EnergyLog* energyLogPtr : energyLogPtrs)
    {
        EnergyLogEntry* logEntryPtr = energyLogPtr->getLog(logType).getFirstEntry(); 
        logEntryPtrs.push_back(logEntryPtr);
        if (logEntryPtr != nullptr) moreEntries = true;
    }

    while (moreEntries)
    {
        // Determine timeslot for the current row
        time_t time = currentTime + 400 * SECONDS_PER_DAY;
        for (EnergyLogEntry* logEntryPtr : logEntryPtrs)
        {
            if (logEntryPtr != nullptr)
                time = std::min(time, logEntryPtr->time);
        }

        writeGraphRow(logEntryPtrs, time, timeFormat, energyDivisor, maxValue);

        moreEntries = false;
        for (int i = 0; i < energyLogPtrs.size(); i++)
        {
            EnergyLogEntry* logEntryPtr = logEntryPtrs[i];
            if ((logEntryPtr != nullptr) && (logEntryPtr->time == time))
            {
                logEntryPtr = energyLogPtrs[i]->getLog(logType).getNextEntry(); 
                logEntryPtrs[i] = logEntryPtr;
            }
            if (logEntryPtr != nullptr) moreEntries = true;
        }
    }

    Html.writeTableEnd();
    Html.writeSectionEnd();
}


void handleHttpRootRequest()
{
    Tracer tracer("handleHttpRootRequest");

    if (WiFiSM.isInAccessPointMode())
    {
        handleHttpConfigFormRequest();
        return;
    }
    
    Html.writeHeader("Home", Nav, pollInterval);

    const char* ftpSync;
    if (!PersistentData.isFTPEnabled())
        ftpSync = "Disabled";
    else if (lastFTPSyncTime == 0)
        ftpSync = "Not yet";
    else
        ftpSync = formatTime("%H:%M", lastFTPSyncTime);

    Html.writeDivStart("flex-container");

    Html.writeSectionStart("Status");
    Html.writeTableStart();
    Html.writeRow("WiFi RSSI", "%d dBm", static_cast<int>(WiFi.RSSI()));
    Html.writeRow("Free Heap", "%0.1f kB", float(ESP.getFreeHeap()) / 1024);
    Html.writeRow("Uptime", "%0.1f days", float(WiFiSM.getUptime()) / SECONDS_PER_DAY);
    Html.writeRow("Next poll", "%d s", pollInvertersTime - currentTime);
    Html.writeRow("FTP Sync", ftpSync);
    Html.writeRow("Sync Entries", "%d / %d", powerLogEntriesToSync, PersistentData.ftpSyncEntries);
    Html.writeTableEnd();
    Html.writeSectionEnd();

    writeTotals();
    writeInverterData();

    int inv = WebServer.hasArg(INVERTER_PARAM) ? WebServer.arg(INVERTER_PARAM).toInt() : -1;

    ChannelType_t channel = WebServer.hasArg(CHANNEL_PARAM)
        ? static_cast<ChannelType_t>(WebServer.arg(CHANNEL_PARAM).toInt())
        : TYPE_AC;

    EnergyLogType logType = WebServer.hasArg(TIMEFRAME_PARAM)
        ? static_cast<EnergyLogType>(WebServer.arg(TIMEFRAME_PARAM).toInt())
        : EnergyLogType::Today;

    writeEnergyLogs(inv, channel, logType);

    Html.writeDivEnd();
    Html.writeFooter();

    WebServer.send(200, ContentTypeHtml, HttpResponse.c_str());
}


void handleHttpSyncFTPRequest()
{
    Tracer tracer("handleHttpSyncFTPRequest");

    Html.writeHeader("FTP Sync", Nav);

    Html.writePreStart();
    bool success = trySyncFTP(&HttpResponse); 
    Html.writePreEnd();

    if (success)
    {
        Html.writeParagraph("Success!");
        syncFTPTime = 0; // Cancel scheduled sync (if any)
    }
    else
        Html.writeParagraph("Failed: %s", FTPClient.getLastError());

    Html.writeHeading("CSV headers", 2);
    Html.writePreStart();
    HttpResponse.print("Time");
    for (int i = 0; i < Hoymiles.getNumInverters(); i++)
    {
        auto inverterPtr = Hoymiles.getInverterByPos(i);
        if (inverterPtr == nullptr) continue;
        for (ChannelNum_t channel : inverterPtr->Statistics()->getChannelsByType(TYPE_DC))
            HttpResponse.printf(F(";%s Pdc%d (W)"), inverterPtr->name(), channel + 1);
        HttpResponse.printf(F(";%s Pac (W)"), inverterPtr->name());
        HttpResponse.printf(F(";%s Vac (V)"), inverterPtr->name());
    }
    HttpResponse.println();
    HttpResponse.println("Date;On time (h);Max Power (W);Energy (kWh)");
    HttpResponse.println("Time;Device;On time (h);Max Power (W);Energy (kWh)");
    Html.writePreEnd();

    Html.writeFooter();

    WebServer.send(200, ContentTypeHtml, HttpResponse.c_str());
}


void handleHttpPowerLogRequest()
{
    Tracer tracer(F("handleHttpOpenThermLogRequest"));

    std::vector<size_t> dcChannels;

    Html.writeHeader("Power log", Nav);
    
    int currentPage = WebServer.hasArg("page") ? WebServer.arg("page").toInt() : 0;
    int totalPages = ((PowerLog.count() - 1) / POWER_LOG_PAGE_SIZE) + 1;
    Html.writePager(totalPages, currentPage);

    Html.writeTableStart();
    Html.writeRowStart();
    Html.writeHeaderCell("Time", 0, 2);
    for (int i = 0; i < Hoymiles.getNumInverters(); i++)
    {
        auto inverterPtr = Hoymiles.getInverterByPos(i);
        if (inverterPtr == nullptr) continue;
        size_t dcChannelCount = inverterPtr->Statistics()->getChannelsByType(TYPE_DC).size();
        dcChannels.push_back(dcChannelCount);
        Html.writeHeaderCell(inverterPtr->name(), dcChannelCount + 2);
    }
    Html.writeRowEnd();
    Html.writeRowStart();
    for (int dcChannelCount : dcChannels)
    {
        for (int ch = 1; ch <= dcChannelCount; ch++)
            HttpResponse.printf(F("<td>P<sub>dc%d</sub> (W)</td>"), ch);
        Html.writeCell("P<sub>ac</sub> (W)");
        Html.writeCell("V<sub>ac</sub> (V)");
    }
    Html.writeRowEnd();

    PowerLogEntry* powerLogEntryPtr = PowerLog.getFirstEntry();
    for (int i = 0; i < (currentPage * POWER_LOG_PAGE_SIZE) && powerLogEntryPtr != nullptr; i++)
    {
        powerLogEntryPtr = PowerLog.getNextEntry();
    }

    for (int j = 0; j < POWER_LOG_PAGE_SIZE && powerLogEntryPtr != nullptr; j++)
    {
        Html.writeRowStart();
        Html.writeCell(formatTime("%a %H:%M", powerLogEntryPtr->time));
        for (int i = 0; i < dcChannels.size(); i++)
        {
            int dcChannelCount = dcChannels[i];
            for (int ch = 0; ch < dcChannelCount; ch++)
                Html.writeCell(powerLogEntryPtr->dcPower[i][ch]);
            Html.writeCell(powerLogEntryPtr->acPower[i]);
            Html.writeCell(powerLogEntryPtr->acVoltage[i]);
        }
        Html.writeRowEnd();

        powerLogEntryPtr = PowerLog.getNextEntry();
    }

    Html.writeTableEnd();
    Html.writeFooter();

    WebServer.send(200, ContentTypeHtml, HttpResponse.c_str());
}


void handleHttpEventLogRequest()
{
    Tracer tracer("handleHttpEventLogRequest");

    Html.writeHeader("Event log", Nav);

    if (WiFiSM.shouldPerformAction("clear"))
    {
        EventLog.clear();
        WiFiSM.logEvent("Event log cleared.");
    }

    const char* event = EventLog.getFirstEntry();
    while (event != nullptr)
    {
        Html.writeDiv("%s", event);
        event = EventLog.getNextEntry();
    }

    Html.writeActionLink("clear", "Clear event log", currentTime, ButtonClass);

    Html.writeFooter();

    WebServer.send(200, ContentTypeHtml, HttpResponse.c_str());
}


void handleHttpConfigFormRequest()
{
    Tracer tracer("handleHttpConfigFormRequest");

    Html.writeHeader("Settings", Nav);

    Html.writeFormStart("/config", "grid");
    PersistentData.writeHtmlForm(Html);
    Html.writeSubmitButton("Save");
    Html.writeFormEnd();

    if (WiFiSM.shouldPerformAction("reset"))
    {
        Html.writeParagraph("Resetting...");
        WiFiSM.reset();
    }
    else
        Html.writeActionLink("reset", "Reset ESP", currentTime, ButtonClass);

    Html.writeFooter();

    WebServer.send(200, ContentTypeHtml, HttpResponse.c_str());
}


void handleHttpConfigFormPost()
{
    Tracer tracer("handleHttpConfigFormPost");

    PersistentData.parseHtmlFormData([](const String& id) -> String { return WebServer.arg(id); });
    PersistentData.validate();
    PersistentData.writeToEEPROM();

    handleHttpConfigFormRequest();
}


void handleHttpInvertersFormRequest()
{
    Tracer tracer("handleHttpInvertersFormRequest");

    Html.writeHeader("Inverters", Nav);

    Html.writeFormStart("/inverters");
    Html.writeTableStart();

    Html.writeRowStart();
    Html.writeHeaderCell("Serial#");
    Html.writeHeaderCell("Name");
    Html.writeHeaderCell("Type");
    Html.writeHeaderCell("P<sub>max</sub> (W)");
    Html.writeHeaderCell("Limit (%)");
    Html.writeRowEnd();

    for (int i = 0; i < PersistentData.registeredInvertersCount; i++)
    {
        RegisteredInverter& registeredInverter = PersistentData.registeredInverters[i];
        const char* inverterSerial = formatSerial(registeredInverter.serial);


        String inverterType = "[Unknown]";
        uint16_t maxPower = 0;
        int limitPercent = 0;
        auto inverterPtr = Hoymiles.getInverterBySerial(registeredInverter.serial);
        if (inverterPtr != nullptr)
        {
            inverterType = inverterPtr->typeName();
            maxPower = inverterPtr->DevInfo()->getMaxPower();
            limitPercent = inverterPtr->SystemConfigPara()->getLimitPercent();
        }

        Html.writeRowStart();
        Html.writeCell(inverterSerial);
        HttpResponse.printf(
            F("<td><input type='text' name='name_%s' value='%s' maxlength='%d'></td>"), 
            inverterSerial,
            registeredInverter.name,
            MAX_INVERTER_NAME_LENGTH - 1);
        HttpResponse.printf(
            F("<td><a href='/gridprofile?inverter=%d'>%s</a></td>"),
            i, inverterType.c_str());
        Html.writeCell(maxPower);
        Html.writeCell(limitPercent);
        Html.writeRowEnd();
    }

    if (PersistentData.registeredInvertersCount < MAX_REGISTERED_INVERTERS)
    {
        Html.writeRowStart();
        HttpResponse.printf(
            F("<td><input type='text' name='new_serial' minlenght='%d' maxlength='%d' placeholder='Enter %d digits'></td>"),
            SERIAL_LENGTH, SERIAL_LENGTH, SERIAL_LENGTH);
        HttpResponse.printf(
            F("<td><input type='text' name='new_name' maxlength='%d' placeholder='Enter name'></td>"), 
            MAX_INVERTER_NAME_LENGTH - 1);
        Html.writeCell("");
        Html.writeCell("");
        Html.writeCell("");
        Html.writeRowEnd();
    }

    Html.writeTableEnd();
    Html.writeSubmitButton("Save");
    Html.writeFormEnd();
    Html.writeFooter();

    WebServer.send(200, ContentTypeHtml, HttpResponse.c_str());
}

void handleHttpInvertersFormPost()
{
    Tracer tracer("handleHttpInvertersFormPost");

    // Rename/remove existing registered inverters
    for (int i = 0; i < WebServer.args(); i++)
    {
        String argName = WebServer.argName(i);
        if (argName.startsWith("name_"))
        {
            String serialString = argName.substring(5);
            String name = WebServer.arg(i);
            uint64_t serial = parseSerial(serialString.c_str());
            TRACE(F("serial='%s', name='%s'\n"), formatSerial(serial), name.c_str());
            int n = PersistentData.getRegisteredInverter(serial);
            if (n >= 0)
            {
                if (name.length() == 0)
                    removeInverter(n);
                else
                {
                    // Rename registered inverter
                    strncpy(PersistentData.registeredInverters[n].name, name.c_str(), MAX_INVERTER_NAME_LENGTH);
                }
            }
            else
                TRACE(F("Not found.\n"));
        }
    }

    // Register new inverter
    String newSerialString = WebServer.arg("new_serial");
    String newName = WebServer.arg("new_name");
    TRACE(F("new_serial='%s', new_name='%s'\n"), newSerialString.c_str(), newName.c_str());
    if ((newSerialString.length() != 0) && (newName.length() != 0) && (PersistentData.registeredInvertersCount < MAX_REGISTERED_INVERTERS))
    {
        uint64_t newSerial = parseSerial(newSerialString.c_str());
        int i = PersistentData.registeredInvertersCount;        
        if (addInverter(newName.c_str(), newSerial))
        {
            PersistentData.registeredInvertersCount++;
            PersistentData.registeredInverters[i].serial = newSerial;
            strncpy(PersistentData.registeredInverters[i].name, newName.c_str(), MAX_INVERTER_NAME_LENGTH);
        }
    }

    PersistentData.writeToEEPROM();
 
    handleHttpInvertersFormRequest();
}


void handleHttpGridProfileRequest()
{
    Tracer tracer("handleHttpGridProfileRequest");

    int inverter = WebServer.hasArg("inverter") ? WebServer.arg("inverter").toInt() : 0;

    Html.writeHeader("Grid Profile", Nav);

    auto inverterPtr = Hoymiles.getInverterByPos(inverter);
    if (inverterPtr != nullptr)
    {
        GridProfileParser* gridProfilePtr = inverterPtr->GridProfile();
        Html.writeHeading(gridProfilePtr->getProfileName(), 2);
        Html.writeTableStart();
        for (GridProfileSection_t& section : gridProfilePtr->getProfile())
        {
            Html.writeRowStart("grid-profile-section");
            Html.writeHeaderCell(section.SectionName, 2);
            Html.writeRowEnd();

            for (GridProfileItem_t item : section.items)
            {
                if (item.Unit.equals("bool"))
                    Html.writeRow(item.Name, "%s", (item.Value == 0) ? "no" : "yes");
                else
                    Html.writeRow(item.Name, "%0.1f %s", item.Value, item.Unit.c_str());
            }
        }
        Html.writeTableEnd();
    }

    Html.writeFooter();

    WebServer.send(200, ContentTypeHtml, HttpResponse.c_str());
}


void handleHttpSmartHomeRequest()
{
    Tracer tracer("handleHttpSmartHomeRequest");

    Html.writeHeader("Smart Home", Nav);
    SmartHome.writeHtml(Html);

    WebServer.send(200, ContentTypeHtml, HttpResponse.c_str());
}
