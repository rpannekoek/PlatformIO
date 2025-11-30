#include <WiFiStateMachine.h>
#include <WiFiFTP.h>
#include <TimeUtils.h>
#include <Tracer.h>
#include <StringBuilder.h>
#include <HtmlWriter.h>
#include <Log.h>
#include <LED.h>
#include <AsyncHTTPRequest_Generic.h>
#include "PersistentData.h"
#include "Aquarea.h"
#include "MonitoredTopics.h"
#include "DayStatsEntry.h"
#include "OTGWClient.h"
#include "SolarPumpControl.h"

constexpr uint8_t SOLAR_PUMP_PWM_PIN = 16;
constexpr uint16_t SOLAR_LOG_SIZE = 50;

constexpr uint16_t HTTP_POLL_INTERVAL = SECONDS_PER_MINUTE;
constexpr uint16_t EVENT_LOG_LENGTH = 50;
constexpr uint16_t TOPIC_LOG_SIZE = 200;
constexpr int TOPIC_LOG_PAGE_SIZE = 50;
constexpr int FTP_TIMEOUT_MS = 2000;
constexpr uint32_t FTP_RETRY_INTERVAL = 15 * SECONDS_PER_MINUTE;
constexpr uint32_t QUERY_AQUAREA_INTERVAL = 6; // seconds
constexpr uint32_t AGGREGATION_INTERVAL = SECONDS_PER_MINUTE;
constexpr int ANTI_FREEZE_DELTA_T = 5;
constexpr uint16_t HP_ON_THRESHOLD = 10;
constexpr float PUMP_FLOW_THRESHOLD = 7.0F;

const char* ContentTypeHtml = "text/html;charset=UTF-8";
const char* ContentTypeText = "text/plain";
const char* ContentTypeJson = "application/json";
const char* ButtonClass = "button";

enum FileId
{
    Logo,
    Styles,
    BinaryIcon,
    GraphIcon,
    HomeIcon,
    ListIcon,
    LogFileIcon,
    SettingsIcon,
    UploadIcon,
    _LastFile
};

const char* Files[] PROGMEM =
{
    "Logo.png",
    "styles.css",
    "Binary.svg",
    "Graph.svg",
    "Home.svg",
    "List.svg",
    "LogFile.svg",
    "Settings.svg",
    "Upload.svg"
};

ESPWebServer WebServer(80); // Default HTTP port
WiFiNTP TimeServer;
WiFiFTPClient FTPClient(FTP_TIMEOUT_MS);
OTGWClient OTGW;
StringBuilder HttpResponse(4 * 1024); // 4 kB HTTP response buffer (we're using chunked responses)
HtmlWriter Html(HttpResponse, Files[Logo], Files[Styles]);
StringLog EventLog(EVENT_LOG_LENGTH, 96);
StaticLog<TopicLogEntry> TopicLog(TOPIC_LOG_SIZE);
StaticLog<DayStatsEntry> DayStats(7);
SimpleLED BuiltinLED(LED_BUILTIN, true);
WiFiStateMachine WiFiSM(BuiltinLED, TimeServer, WebServer, EventLog);
Aquarea HeatPump;
SolarPumpControl SolarPump(SOLAR_PUMP_PWM_PIN, SOLAR_LOG_SIZE);
Navigation Nav;

TopicLogEntry newTopicLogEntry;
int topicLogAggregations = 0;

TopicLogEntry* lastTopicLogEntryPtr = nullptr;
DayStatsEntry* lastDayStatsEntryPtr = nullptr;

uint16_t ftpSyncEntries = 0;
uint16_t heatPumpOnCount = 0;
bool isDefrosting = false;
bool antiFreezeActivated = false;
bool testAntiFreeze = false;
bool testDefrost = false;
float testSolarDeltaT = 0;
bool pumpPrime = false;
int otgwAttempt = 0;

time_t currentTime = 0;
time_t queryAquareaTime = 0;
time_t lastPacketReceivedTime = 0;
time_t lastPacketErrorTime = 0;
time_t topicLogAggregationTime = 0;
time_t syncFTPTime = 0;
time_t lastFTPSyncTime = 0;

// Forward defines
void handleHttpConfigFormRequest();


void newDayStatsEntry()
{
    time_t startOfDay = currentTime - (currentTime % SECONDS_PER_DAY);
    DayStatsEntry newDayStatsEntry;
    newDayStatsEntry.startTime = startOfDay;
    newDayStatsEntry.stopTime = startOfDay;
    lastDayStatsEntryPtr = DayStats.add(&newDayStatsEntry);
}


bool otgwSetPump(bool on, const String& reason = "")
{
    if (on)
        WiFiSM.logEvent(F("OTGW Pump resume"));
    else
        WiFiSM.logEvent(F("OTGW Pump off: %s"), reason.c_str());

    otgwAttempt = 0;
    if (OTGW.setPump(on, reason) != HTTP_REQUEST_PENDING)
    {
        WiFiSM.logEvent(F("OTGW: %s"), OTGW.getLastError().c_str());
        return false;
    }
    return true;
}


void otgwPumpControl(bool pumpIsOn, int defrostingState)
{
    if ((defrostingState != 0) && !isDefrosting)
    {
        // Defrost started
        otgwSetPump(false, F("Defrost"));
    }
    else if ((defrostingState == 0) && isDefrosting)
    {
        // Defrost stopped
        otgwSetPump(true);
    }
    else if (pumpIsOn && (heatPumpOnCount == 0) && !isDefrosting && !antiFreezeActivated && !pumpPrime)
    {
        // Pump is on, but compressor not yet: "pump prime"
        pumpPrime = true;
        otgwSetPump(false, F("Prime"));
    }
    else if ((heatPumpOnCount == HP_ON_THRESHOLD) && pumpPrime)
    {
        // Compressor is on for 1 minute; pump prime finished
        pumpPrime = false;
        otgwSetPump(true);
    }
}


void antiFreezeControl(float inletTemp, float outletTemp, float compPower)
{
    int antiFreezeOnTemp = PersistentData.antiFreezeTemp;
    int antiFreezeOffTemp = antiFreezeOnTemp + ANTI_FREEZE_DELTA_T;

    if (!antiFreezeActivated)
    {
        // Anti-freeze control is not active (yet)
        if ((std::min(inletTemp, outletTemp) <= antiFreezeOnTemp) || testAntiFreeze)
        {
            antiFreezeActivated = true;
            WiFiSM.logEvent(F("Anti-freeze activated."));
            if (!HeatPump.setPump(true))
                WiFiSM.logEvent(F("Unable to activate pump."));
        }  
    }
    else
    {
        // Anti-freeze control is active
        if ((std::min(inletTemp, outletTemp) > antiFreezeOffTemp) && !testAntiFreeze)
        {
            antiFreezeActivated = false;
            WiFiSM.logEvent(F("Anti-freeze deactivated."));

            // Don't stop pump if compressor started in the meantime.
            if (compPower == 0)
            {
                if (!HeatPump.setPump(false))
                    WiFiSM.logEvent(F("Unable to deactivate pump."));
            }
            else
                WiFiSM.logEvent(F("Compressor is on."));
        }  
    }
}


void updateDayStats(uint32_t secondsSinceLastUpdate, float powerInKW, float powerOutKW, int defrostingState)
{
    if ((currentTime / SECONDS_PER_DAY) > (lastDayStatsEntryPtr->startTime / SECONDS_PER_DAY))
    {
        newDayStatsEntry();
        HeatPump.resetPacketStats();
    }

    lastDayStatsEntryPtr->update(currentTime, secondsSinceLastUpdate, powerInKW, powerOutKW, antiFreezeActivated);

    if (defrostingState != 0 && !isDefrosting)
        lastDayStatsEntryPtr->defrosts++; // Defrost started

    if (heatPumpOnCount == HP_ON_THRESHOLD)
        lastDayStatsEntryPtr->onCount++;
}


void updateTopicLog()
{
    int i = 0;
    for (MonitoredTopic& topic : MonitoredTopics)
        newTopicLogEntry.topicValues[i++] += HeatPump.getTopic(topic.id).getValue().toFloat();

    topicLogAggregations++;
    if (currentTime < topicLogAggregationTime) return; 

    // Calculate averages
    for (int i = 0; i < NUMBER_OF_MONITORED_TOPICS; i++)
        newTopicLogEntry.topicValues[i] /= topicLogAggregations;

    if ((lastTopicLogEntryPtr == nullptr) || !newTopicLogEntry.equals(lastTopicLogEntryPtr))
    {
        lastTopicLogEntryPtr = TopicLog.add(&newTopicLogEntry);

        newTopicLogEntry.time = currentTime;

        ftpSyncEntries = std::min(uint16_t(ftpSyncEntries + 1), TOPIC_LOG_SIZE);
        if (PersistentData.isFTPEnabled() && ftpSyncEntries == PersistentData.ftpSyncEntries)
            syncFTPTime = currentTime;
    }

    newTopicLogEntry.reset(); // Keep moving average of last minute only
    topicLogAggregations = 0;
    topicLogAggregationTime += AGGREGATION_INTERVAL;
}


void handleNewAquareaData()
{
    Tracer tracer(F("handleNewAquareaData"));

    uint32_t secondsSinceLastUpdate = (lastPacketReceivedTime == 0) ? 0 : (currentTime - lastPacketReceivedTime);
    lastPacketReceivedTime = currentTime;

    float inletTemp = HeatPump.getTopic(TopicId::Main_Inlet_Temp).getValue().toFloat();
    float outletTemp = HeatPump.getTopic(TopicId::Main_Outlet_Temp).getValue().toFloat();
    float compPower = HeatPump.getTopic(TopicId::Compressor_Power).getValue().toFloat();
    float heatPower = HeatPump.getTopic(TopicId::Heat_Power).getValue().toFloat();
    int defrostingState = HeatPump.getTopic(TopicId::Defrosting_State).getValue().toInt();
    float pumpFlow = HeatPump.getTopic(TopicId::Pump_Flow).getValue().toFloat();
    int solarDeltaT = HeatPump.getTopic(TopicId::Solar_DeltaT).getValue().toInt();
    int solarOnDeltaT = HeatPump.getTopic(TopicId::Solar_On_Delta).getValue().toInt();
    int solarOffDeltaT = HeatPump.getTopic(TopicId::Solar_Off_Delta).getValue().toInt();

    if (testDefrost) defrostingState = 1;

    bool pumpIsOn = pumpFlow >= PUMP_FLOW_THRESHOLD;
    if (!pumpIsOn)
        heatPumpOnCount = 0;
    else if (compPower > 0)
        heatPumpOnCount++;

    if (OTGW.isInitialized && WiFiSM.isConnected())
        otgwPumpControl(pumpIsOn, defrostingState);
    if ((PersistentData.solarPumpPWMDeltaT > 0) && (testSolarDeltaT == 0.0F))
    {
        SolarPump.control(
            solarDeltaT,
            solarOnDeltaT,
            solarOffDeltaT,
            PersistentData.solarPumpPWMDeltaT,
            PersistentData.solarPumpPWMChangeRatePct);
        SolarPump.updateLog(currentTime, solarDeltaT);
    }
    antiFreezeControl(inletTemp, outletTemp, compPower);
    updateDayStats(secondsSinceLastUpdate, compPower, heatPower, defrostingState);
    updateTopicLog();

    isDefrosting = defrostingState != 0;
}


void writeTopicLogCsv(uint16_t count, Print& destination)
{
    for (auto i = TopicLog.at(-count); i != TopicLog.end(); ++i)
    {
        TopicLogEntry& logEntry = *i;

        destination.print(formatTime("%F %H:%M", logEntry.time));
        int t = 0;
        for (MonitoredTopic& topic : MonitoredTopics)
        {
            destination.print(";");
            destination.print(topic.formatValue(logEntry.topicValues[t++], false, 1));
        }
        destination.println();
    }
}


bool trySyncFTP(Print* printTo)
{
    Tracer tracer(F("trySyncFTP"));

    char filename[40];
    snprintf(filename, sizeof(filename), "%s.csv", PersistentData.hostName);

    if (!FTPClient.begin(
        PersistentData.ftpServer,
        PersistentData.ftpUser,
        PersistentData.ftpPassword,
        FTP_DEFAULT_CONTROL_PORT,
        printTo))
    {
        return false;
    }

    bool success = false;
    WiFiClient& dataClient = FTPClient.append(filename);
    if (dataClient.connected())
    {
        if (ftpSyncEntries > 0)
        {
            writeTopicLogCsv(ftpSyncEntries, dataClient);
            ftpSyncEntries = 0;
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

    FTPClient.end();

    return success;
}


void writeCurrentValues()
{
    Html.writeSectionStart(F("Current values"));
    Html.writeTableStart();
    for (MonitoredTopic& topic: MonitoredTopics)
    {
        float topicValue = HeatPump.getTopic(topic.id).getValue().toFloat();

        String barCssClass = topic.style;
        barCssClass += "Bar";

        Html.writeRowStart();
        Html.writeHeaderCell(FPSTR(topic.htmlLabel));
        Html.writeCell(topic.formatValue(topicValue, true));
        Html.writeGraphCell(topicValue, topic.minValue, topic.maxValue, barCssClass, true);
        Html.writeRowEnd();
    }

    SolarPump.writeStateRow(Html);

    Html.writeTableEnd();
    Html.writeSectionEnd();
}


void writeStatisticsPerDay()
{
    // Auto-ranging: determine max energy
    float maxEnergy = 0.1; // Prevent division by zero
    for (DayStatsEntry& dayStatsEntry : DayStats)
        maxEnergy = std::max(maxEnergy, dayStatsEntry.energyOut);

    Html.writeSectionStart(F("Statistics per day"));
    Html.writeTableStart();
    DayStatsEntry::writeHeader(Html);
    for (DayStatsEntry& dayStatsEntry : DayStats)
        dayStatsEntry.writeRow(Html, maxEnergy);
    Html.writeTableEnd();
    Html.writeSectionEnd();
}


void handleHttpRootRequest()
{
    Tracer tracer(F("handleHttpRootRequest"));

    if (WiFiSM.isInAccessPointMode())
    {
        handleHttpConfigFormRequest();
        return;
    }

    String ftpSync;
    if (!PersistentData.isFTPEnabled())
        ftpSync = F("Disabled");
    else if (lastFTPSyncTime == 0)
        ftpSync = F("Not yet");
    else
        ftpSync = formatTime("%H:%M", lastFTPSyncTime);

    const char* lastPacket = (lastPacketReceivedTime == 0)
        ? "Not yet"
        : formatTime("%H:%M:%S", lastPacketReceivedTime);

    ChunkedResponse response(HttpResponse, WebServer, ContentTypeHtml);
    Html.writeHeader(F("Home"), Nav, HTTP_POLL_INTERVAL);

    Html.writeDivStart(F("flex-container"));

    Html.writeSectionStart(F("Status"));
    Html.writeTableStart();
    Html.writeRow(F("WiFi RSSI"), F("%d dBm"), static_cast<int>(WiFi.RSSI()));
    Html.writeRow(F("Free Heap"), F("%0.1f kB"), float(ESP.getFreeHeap()) / 1024);
    Html.writeRow(F("Uptime"), F("%0.1f days"), float(WiFiSM.getUptime()) / SECONDS_PER_DAY);
    Html.writeRow(F("Aquarea"), HeatPump.getTopic(TopicId::Error).getValue());
    Html.writeRow(F("Last packet"), lastPacket);
    Html.writeRow(F("Packet errors"), F("%0.1f %%"), HeatPump.getPacketErrorRatio() * 100);
    Html.writeRow(F("FTP Sync"), ftpSync);
    Html.writeRow(F("Sync entries"), F("%d / %d"), ftpSyncEntries, PersistentData.ftpSyncEntries);
    Html.writeTableEnd();
    Html.writeSectionEnd();

    if (lastPacketReceivedTime != 0)
        writeCurrentValues();
    
    writeStatisticsPerDay();

    Html.writeDivEnd();
    Html.writeFooter();
}


void handleHttpTopicsRequest()
{
    Tracer tracer(F("handleHttpTopicsRequest"));
    ChunkedResponse response(HttpResponse, WebServer, ContentTypeHtml);

    Html.writeHeader(F("Topics"), Nav);

    if (lastPacketReceivedTime != 0)
    {
        Html.writeParagraph(
            F("Packet received @ %s"),
            formatTime("%H:%M:%S", lastPacketReceivedTime));

        Html.writeTableStart();

        for (TopicId topicId : HeatPump.getAllTopicIds())
        {
            Topic topic = HeatPump.getTopic(topicId);

            Html.writeRowStart();
            Html.writeHeaderCell(topic.getName());
            Html.writeCell(topic.getValue());
            Html.writeCell(topic.getDescription());
            Html.writeRowEnd();
        }

        Html.writeTableEnd();
    }

    Html.writeFooter();
}


void handleHttpTopicLogRequest()
{
    Tracer tracer(F("handleHttpTopicLogRequest"));
    ChunkedResponse response(HttpResponse, WebServer, ContentTypeHtml);

    int currentPage = WebServer.hasArg("page") ? WebServer.arg("page").toInt() : 0;
    int totalPages = ((TopicLog.count() - 1) / TOPIC_LOG_PAGE_SIZE) + 1;

    Html.writeHeader(F("Aquarea log"), Nav);
    Html.writePager(totalPages, currentPage);
    Html.writeTableStart();

    Html.writeRowStart();
    Html.writeHeaderCell(F("Time"));
    for (MonitoredTopic& topic: MonitoredTopics)
        Html.writeHeaderCell(FPSTR(topic.htmlLabel));
    Html.writeRowEnd();

    int n = 0;
    for (auto i = TopicLog.at(currentPage * TOPIC_LOG_PAGE_SIZE); i != TopicLog.end(); ++i)
    {
        TopicLogEntry& entry = *i;

        Html.writeRowStart();
        Html.writeCell(formatTime("%H:%M", entry.time));
        int t = 0;
        for (MonitoredTopic& topic : MonitoredTopics)
            Html.writeCell(topic.formatValue(entry.topicValues[t++], false, 1));
        Html.writeRowEnd();

        if (++n == TOPIC_LOG_PAGE_SIZE) break;
    }

    Html.writeTableEnd();
    Html.writeFooter();
}


void handleHttpSolarLogRequest()
{
    Tracer tracer(F("handleHttpSolarLogRequest"));
    ChunkedResponse response(HttpResponse, WebServer, ContentTypeHtml);

    Html.writeHeader(F("Solar log"), Nav);

    Html.writeTableStart();
    Html.writeRowStart();
    Html.writeHeaderCell(F("Time"));
    Html.writeHeaderCell(F("ΔT"));
    Html.writeHeaderCell(F("Duty"));
    Html.writeHeaderCell(F("Target"));
    Html.writeRowEnd();

    for (SolarLogEntry& logEntry : SolarPump.Log)
    {
        Html.writeRowStart();
        Html.writeCell(formatTime("%H:%M:%S", logEntry.time));
        Html.writeCell(logEntry.deltaT);
        Html.writeCell(F("%0.0f %%"), logEntry.dutyCycle * 100);
        Html.writeCell(F("%0.0f %%"), logEntry.targetDutyCycle * 100);
        Html.writeRowEnd();
    }

    Html.writeTableEnd();
    Html.writeFooter();
}


void handleHttpHexDumpRequest()
{
    Tracer tracer(F("handleHttpHexDumpRequest"));

    if (WebServer.hasArg("raw"))
    {
        HttpResponse.clear();
        HeatPump.writeHexDump(HttpResponse, false);

        WebServer.send(200, ContentTypeText, HttpResponse);
    }
    else
    {
        ChunkedResponse response(HttpResponse, WebServer, ContentTypeHtml);
        Html.writeHeader(F("Hex dump"), Nav);

        Html.writeParagraph(
            F("Received %u valid packets, %u repaired packets, %u invalid packets."),
            HeatPump.getValidPackets(),
            HeatPump.getRepairedPackets(),
            HeatPump.getInvalidPackets());

        Html.writeParagraph(
            F("Last valid packet @ %s"),
            formatTime("%H:%M:%S", lastPacketReceivedTime));

        Html.writeParagraph(
            F("Last error @ %s : %s"),
            formatTime("%H:%M:%S", lastPacketErrorTime),
            HeatPump.getLastError().c_str());

        if (lastPacketReceivedTime != 0)
        {
            Html.writeHeading(F("Last valid packet"), 2);

            Html.writePreStart();
            HeatPump.writeHexDump(HttpResponse, false);
            Html.writePreEnd();
        }

        Html.writeHeading(F("Last invalid packet"), 2);

        Html.writePreStart();
        HeatPump.writeHexDump(HttpResponse, true);
        Html.writePreEnd();

        Html.writeFooter();
    }
}


void handleHttpTestRequest()
{
    Tracer tracer(F("handleHttpTestRequest"));
    ChunkedResponse response(HttpResponse, WebServer, ContentTypeHtml);

    Html.writeHeader(F("Test"), Nav);

    if (WiFiSM.shouldPerformAction(F("antiFreeze")))
    {
        testAntiFreeze = !testAntiFreeze;
        const char* switchState = testAntiFreeze ? "on" : "off"; 
        Html.writeParagraph(F("Anti-freeze test: %s"), switchState);
    }

    if (WiFiSM.shouldPerformAction(F("defrost")))
    {
        testDefrost = !testDefrost;
        handleNewAquareaData();
        const char* switchState = testDefrost ? "on" : "off"; 
        Html.writeParagraph(F("Defrost test: %s"), switchState);
    }

    if (WiFiSM.shouldPerformAction(F("solarPWM")))
    {
        const int testSolarOnDeltaT = 7;
        const int testSolarOffDeltaT = 3;
        if (testSolarDeltaT <= testSolarOnDeltaT)
            testSolarDeltaT += 1.0F;
        else if (testSolarDeltaT <= (testSolarOffDeltaT + PersistentData.solarPumpPWMDeltaT))
            testSolarDeltaT += 0.5F;
        else
            testSolarDeltaT = 0.0F;
        SolarPump.control(
            testSolarDeltaT,
            testSolarOnDeltaT,
            testSolarOffDeltaT,
            PersistentData.solarPumpPWMDeltaT,
            PersistentData.solarPumpPWMChangeRatePct);
        SolarPump.updateLog(currentTime, testSolarDeltaT);
        Html.writeParagraph(F("Solar DeltaT: %d"), int(testSolarDeltaT));
        Html.writeTableStart();
        SolarPump.writeStateRow(Html);
        Html.writeTableEnd();
    }
    
    Html.writeActionLink(F("antiFreeze"), F("Test anti-freeze (switch)"), currentTime, ButtonClass);
    Html.writeActionLink(F("defrost"), F("Test defrost (switch)"), currentTime, ButtonClass);
    Html.writeActionLink(F("solarPWM"), F("Test solar pump PWM"), currentTime, ButtonClass);

    if (WiFiSM.shouldPerformAction("pump"))
    {
        bool success = otgwSetPump(true);
        Html.writeParagraph("%d", success);
    }
    else
        Html.writeActionLink("pump", "Pump", currentTime, ButtonClass);

    if (WiFiSM.shouldPerformAction(F("fillDayStats")))
    {
        for (int i = 0; i < 6; i++)
        {
            float powerOutKW = float(i); 
            float powerInKW = powerOutKW / 4;
            lastDayStatsEntryPtr->update(currentTime + i * SECONDS_PER_DAY, 3600, powerInKW, powerOutKW, i % 2 == 0);
            newDayStatsEntry();
        }
        Html.writeParagraph(F("Day Stats filled."));
    }
    else
        Html.writeActionLink(F("fillDayStats"), F("Fill Day Stats"), currentTime, ButtonClass);

    if (WiFiSM.shouldPerformAction(F("fillTopicLog")))
    {
        for (int i = 0; i < TOPIC_LOG_SIZE; i++)
        {
            newTopicLogEntry.time = currentTime + (i * 60);
            lastTopicLogEntryPtr = TopicLog.add(&newTopicLogEntry);
        }
        Html.writeParagraph(F("Topic Log filled."));
    }
    else
        Html.writeActionLink(F("fillTopicLog"), F("Fill Topic Log"), currentTime, ButtonClass);

    if (WiFiSM.shouldPerformAction(F("fillEventLog")))
    {
        for (int i = 0; i < EVENT_LOG_LENGTH; i++)
        {
            WiFiSM.logEvent(F("Test event to fill the event log"));
        }
        Html.writeParagraph(F("Event Log filled."));
    }
    else
        Html.writeActionLink(F("fillEventLog"), F("Fill Event Log"), currentTime, ButtonClass);

    Html.writeFooter();
}


void handleHttpAquaMonJsonRequest()
{
    Tracer tracer(F("handleHttpAquaMonJsonRequest"));

    HttpResponse.clear();
    HttpResponse.print(F("{ "));

    bool first = false;
    for (MonitoredTopic& topic : MonitoredTopics)
    {
        String label = FPSTR(topic.label);
        float topicValue = HeatPump.getTopic(topic.id).getValue().toFloat();

        if (first)
            first = false;
        else
            HttpResponse.print(F(", "));
        HttpResponse.printf(
            F(" \"%s\": %s"),
            label.c_str(),
            topic.formatValue(topicValue, false));
    }

    HttpResponse.println(F(" }"));

    WebServer.send(200, ContentTypeJson, HttpResponse);
}


void handleHttpFtpSyncRequest()
{
    Tracer tracer(F("handleHttpFtpSyncRequest"));
    ChunkedResponse response(HttpResponse, WebServer, ContentTypeHtml);

    Html.writeHeader(F("FTP Sync"), Nav);

    Html.writePreStart();
    bool success = trySyncFTP(&HttpResponse); 
    Html.writePreEnd();

    if (success)
    {
        Html.writeParagraph(F("Success!"));
        syncFTPTime = 0; // Cancel scheduled sync (if any)
    }
    else
        Html.writeParagraph(F("Failed: %s"), FTPClient.getLastError());

    Html.writeHeading(F("CSV header"), 2);
    Html.writePreStart();
    HttpResponse.print("Time");
    for (MonitoredTopic& topic : MonitoredTopics)
    {

        HttpResponse.print(";");
        HttpResponse.print(topic.label);
    }
    Html.writePreEnd();

    Html.writeFooter();
}


void handleHttpEventLogRequest()
{
    Tracer tracer(F("handleHttpEventLogRequest"));
    ChunkedResponse response(HttpResponse, WebServer, ContentTypeHtml);

    Html.writeHeader(F("Event log"), Nav);

    if (WiFiSM.shouldPerformAction(F("clear")))
    {
        EventLog.clear();
        WiFiSM.logEvent(F("Event log cleared."));
    }

    for (const char* event : EventLog)
        Html.writeDiv(F("%s"), event);

    Html.writeActionLink(F("clear"), F("Clear event log"), currentTime, ButtonClass);

    Html.writeFooter();
}


void handleHttpConfigFormRequest()
{
    Tracer tracer(F("handleHttpConfigFormRequest"));
    ChunkedResponse response(HttpResponse, WebServer, ContentTypeHtml);

    Html.writeHeader(F("Settings"), Nav);

    Html.writeFormStart(F("/config"), F("grid"));
    PersistentData.writeHtmlForm(Html);
    Html.writeSubmitButton(F("Save"));
    Html.writeFormEnd();

    if (WiFiSM.shouldPerformAction(F("reset")))
    {
        Html.writeParagraph(F("Resetting..."));
        WiFiSM.reset();
    }
    else
        Html.writeActionLink(F("reset"), F("Reset ESP"), currentTime, ButtonClass);

    Html.writeFooter();
}


void handleHttpConfigFormPost()
{
    Tracer tracer(F("handleHttpConfigFormPost"));

    PersistentData.parseHtmlFormData([](const String& id) -> const String { return WebServer.arg(id); });
    PersistentData.validate();
    PersistentData.writeToEEPROM();

    HeatPump.setZone1Offset(PersistentData.zone1Offset);

    handleHttpConfigFormRequest();
}


void onTimeServerSynced()
{
    queryAquareaTime = currentTime;
    topicLogAggregationTime = currentTime + AGGREGATION_INTERVAL;

    newTopicLogEntry.time = currentTime;
    newTopicLogEntry.reset();

    newDayStatsEntry();
}


void onWiFiInitialized()
{
    if (Serial.available())
    {
        BuiltinLED.setOn(true);
        if (HeatPump.readPacket())
            handleNewAquareaData();
        else
        {
            lastPacketErrorTime = currentTime;
            if (PersistentData.logPacketErrors)
                WiFiSM.logEvent(HeatPump.getLastError());
        }
        BuiltinLED.setOn(false);
    }

    if (currentTime >= queryAquareaTime)
    {
        queryAquareaTime += QUERY_AQUAREA_INTERVAL;
        if (!HeatPump.sendQuery())
            WiFiSM.logEvent(F("Failed sending Aquarea query"));
    }

    if (!WiFiSM.isConnected())
        return;

    if (OTGW.isInitialized && OTGW.isResponsePending())
    {
        int otgwResult = OTGW.requestData();
        if (otgwResult == HTTP_OK)
            WiFiSM.logEvent(F("OTGW: '%s'"), OTGW.boilerLevel);
        else if (otgwResult != HTTP_REQUEST_PENDING)
        {
            WiFiSM.logEvent(F("OTGW: %s (#%d)"), OTGW.getLastError().c_str(), ++otgwAttempt);
            if (otgwAttempt < 3)
                OTGW.retry();
        }
    }

    if ((syncFTPTime != 0) && (currentTime >= syncFTPTime))
    {
        if (trySyncFTP(nullptr))
        {
            WiFiSM.logEvent(F("FTP sync"));
            syncFTPTime = 0;
        }
        else
        {
            WiFiSM.logEvent(F("FTP sync failed: %s"), FTPClient.getLastError());
            syncFTPTime = currentTime + FTP_RETRY_INTERVAL;
        }
    }
}

// Boot code
void setup() 
{
    Serial.begin(74880); // Use same baudrate as bootloader (will be switched by HeatPump.begin)
    Serial.println("Boot"); // Flush any garbage caused by ESP boot output.

    #ifdef DEBUG_ESP_PORT
    Tracer::traceTo(DEBUG_ESP_PORT);
    Tracer::traceFreeHeap();
    #endif

    BuiltinLED.begin();

    PersistentData.begin();
    TimeServer.NTPServer = PersistentData.ntpServer;
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
            .icon = Files[LogFileIcon],
            .label = PSTR("Event log"),
            .urlPath =PSTR("events"),
            .handler = handleHttpEventLogRequest
        },
        MenuItem
        {
            .icon = Files[GraphIcon],
            .label = PSTR("Aquarea log"),
            .urlPath = PSTR("topiclog"),
            .handler = handleHttpTopicLogRequest
        },
        MenuItem
        {
            .icon = Files[GraphIcon],
            .label = PSTR("Solar log"),
            .urlPath = PSTR("solar"),
            .handler = handleHttpSolarLogRequest
        },
        MenuItem
        {
            .icon = Files[UploadIcon],
            .label = PSTR("FTP Sync"),
            .urlPath = PSTR("sync"),
            .handler= handleHttpFtpSyncRequest
        },
        MenuItem
        {
            .icon = Files[ListIcon],
            .label = PSTR("Topics"),
            .urlPath = PSTR("topic"),
            .handler= handleHttpTopicsRequest
        },
        MenuItem
        {
            .icon = Files[BinaryIcon],
            .label = PSTR("Hex dump"),
            .urlPath = PSTR("hexdump"),
            .handler= handleHttpHexDumpRequest
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

    WebServer.on("/test", handleHttpTestRequest);
    WebServer.on("/json", handleHttpAquaMonJsonRequest);

    WiFiSM.registerStaticFiles(Files, _LastFile);    
    WiFiSM.on(WiFiInitState::TimeServerSynced, onTimeServerSynced);
    WiFiSM.on(WiFiInitState::Initialized, onWiFiInitialized);
    WiFiSM.scanAccessPoints();
    WiFiSM.begin(PersistentData.wifiSSID, PersistentData.wifiKey, PersistentData.hostName);

    Tracer::traceFreeHeap();

    HeatPump.setZone1Offset(PersistentData.zone1Offset);
    HeatPump.begin();

    if (PersistentData.otgwHost[0] != 0)
    {
        OTGW.begin(PersistentData.otgwHost);
    }

    SolarPump.begin();
 
    BuiltinLED.setOff();
}


// Called repeatedly
void loop() 
{
    currentTime = WiFiSM.getCurrentTime();

    // Let WiFi State Machine handle initialization and web requests
    // This also calls the onXXX methods below
    WiFiSM.run();
}
