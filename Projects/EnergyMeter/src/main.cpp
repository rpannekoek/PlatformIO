#include <Arduino.h>
#include <ESPCoreDump.h>
#include <ESPWiFi.h>
#include <ESPWebServer.h>
#include <ESPFileSystem.h>
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
StringBuilder HttpResponse(16 * 1024); // 16KB HTTP response buffer
HtmlWriter Html(HttpResponse, Files[Logo], Files[Styles], MAX_BAR_LENGTH);
Log<const char> EventLog(MAX_EVENT_LOG_SIZE);
SimpleLED BuiltinLED(LED_BUILTIN);
WiFiStateMachine WiFiSM(BuiltinLED, TimeServer, WebServer, EventLog);
Navigation Nav;

StaticLog<PowerLogEntry> PowerLog(POWER_LOG_SIZE);
PowerLogEntry* lastPowerLogEntryPtr = nullptr;
PowerLogEntry newPowerLogEntry;
int powerLogAggregations = 0;
int powerLogEntriesToSync = 0;
bool ftpSyncEnergy = false;

EnergyLog TotalEnergyLog;

time_t currentTime = 0;
time_t syncFTPTime = 0;
time_t lastFTPSyncTime = 0;

void handleSerialRequest()
{
    Tracer tracer("handleSerialRequest");
    Serial.setTimeout(100);

    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    Serial.println(cmd);
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
            // TODO: writePowerLogEntriesCsv(dataClient);
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

    FTPClient.end();
    return success;
}


void onTimeServerSynced()
{
    currentTime = TimeServer.getCurrentTime();
}


void onWiFiInitialized()
{
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


void handleHttpRootRequest()
{
    Tracer tracer("handleHttpRootRequest");

    if (WiFiSM.isInAccessPointMode())
    {
        handleHttpConfigFormRequest();
        return;
    }
    
    Html.writeHeader("Home", Nav, SECONDS_PER_MINUTE);

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
    Html.writeRow("FTP Sync", ftpSync);
    Html.writeRow("Sync Entries", "%d / %d", powerLogEntriesToSync, PersistentData.ftpSyncEntries);
    Html.writeTableEnd();
    Html.writeSectionEnd();

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
    Html.writePreEnd();

    Html.writeFooter();

    WebServer.send(200, ContentTypeHtml, HttpResponse.c_str());
}


void handleHttpPowerLogRequest()
{
    Tracer tracer(F("handleHttpOpenThermLogRequest"));

    Html.writeHeader("Power log", Nav);
    
    int currentPage = WebServer.hasArg("page") ? WebServer.arg("page").toInt() : 0;
    int totalPages = ((PowerLog.count() - 1) / POWER_LOG_PAGE_SIZE) + 1;
    Html.writePager(totalPages, currentPage);

    Html.writeTableStart();
    // TODO
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
            .icon = Files[SettingsIcon],
            .label = PSTR("Settings"),
            .urlPath =PSTR("config"),
            .handler = handleHttpConfigFormRequest,
            .postHandler = handleHttpConfigFormPost
        },
    };
    Nav.registerHttpHandlers(WebServer);

    WiFiSM.registerStaticFiles(Files, _LastFile);
    WiFiSM.on(WiFiInitState::TimeServerSynced, onTimeServerSynced);
    WiFiSM.on(WiFiInitState::Initialized, onWiFiInitialized);
    WiFiSM.scanAccessPoints();
    WiFiSM.begin(PersistentData.wifiSSID, PersistentData.wifiKey, PersistentData.hostName);

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
