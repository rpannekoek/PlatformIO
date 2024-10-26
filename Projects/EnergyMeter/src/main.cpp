#include <iot_main.h>
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
SimpleLED BuiltinLED(LED_BUILTIN, true);
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

IOT_CTX* iotContext = nullptr;
IOT_CAP_HANDLE* iotPowerMeterCapHandle = nullptr;
IOT_CAP_HANDLE* iotEnergyMeterCapHandle = nullptr;
IOT_CAP_HANDLE* iotPowerConsumptionReportCapHandle = nullptr;

double power = 66.6;
float energyTotal = 0;
float energyDelta = 0;
time_t powerConsumptionReportStartTime;
time_t powerChangeTime = 0;


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


static void onSmartThingsStatusUpdate(iot_status_t status, iot_stat_lv_t stat_lv, void *usr_data)
{
    TRACE("SmartThings status: %d.%d\n", status, stat_lv);
}


static void onSmartThingsNotification(iot_noti_data_t *noti_data, void *noti_usr_data)
{
    TRACE("Notification message received\n");

    if (noti_data->type == IOT_NOTI_TYPE_DEV_DELETED) {
        TRACE("[device deleted]\n");
    } else if (noti_data->type == IOT_NOTI_TYPE_RATE_LIMIT) {
        TRACE(
            "[rate limit] Remaining time:%d, sequence number:%d\n",
            noti_data->raw.rate_limit.remainingTime,
            noti_data->raw.rate_limit.sequenceNumber);
    } else if(noti_data->type == IOT_NOTI_TYPE_PREFERENCE_UPDATED) {
		for (int i = 0; i < noti_data->raw.preferences.preferences_num; i++) {
			TRACE("[preference update] name : %s value : ", noti_data->raw.preferences.preferences_data[i].preference_name);
			if (noti_data->raw.preferences.preferences_data[i].preference_data.type == IOT_CAP_VAL_TYPE_NULL)
				TRACE("NULL\n");
			else if (noti_data->raw.preferences.preferences_data[i].preference_data.type == IOT_CAP_VAL_TYPE_STRING)
				TRACE("%s\n", noti_data->raw.preferences.preferences_data[i].preference_data.string);
			else if (noti_data->raw.preferences.preferences_data[i].preference_data.type == IOT_CAP_VAL_TYPE_NUMBER)
				TRACE("%f\n", noti_data->raw.preferences.preferences_data[i].preference_data.number);
			else if (noti_data->raw.preferences.preferences_data[i].preference_data.type == IOT_CAP_VAL_TYPE_INTEGER)
				TRACE("%d\n", noti_data->raw.preferences.preferences_data[i].preference_data.integer);
			else if (noti_data->raw.preferences.preferences_data[i].preference_data.type == IOT_CAP_VAL_TYPE_BOOLEAN)
				TRACE("%s\n", noti_data->raw.preferences.preferences_data[i].preference_data.boolean ? "true" : "false");
			else
				TRACE("Unknown type\n");
		}
	}
}


void sendPower()
{
    Tracer tracer("sendPower");

    int sequenceNr = 0;
	ST_CAP_SEND_ATTR_NUMBER(iotPowerMeterCapHandle, "power", power, "W", nullptr, sequenceNr);
    TRACE("Sequence #%d\n", sequenceNr);
}

void sendEnergy()
{
    Tracer tracer("sendEnergy");

    int sequenceNr = 0;
	ST_CAP_SEND_ATTR_NUMBER(iotEnergyMeterCapHandle, "energy", energyTotal, "Wh", nullptr, sequenceNr);
    TRACE("Sequence #%d\n", sequenceNr);
}

void sendPowerConsumption()
{
    Tracer tracer("sendPowerConsumption");

    time_t currentTime = TimeServer.getCurrentTime();
    char startTimestamp[32];
    strftime(startTimestamp, sizeof(startTimestamp), "%FT%H:%M:%SZ", gmtime(&powerConsumptionReportStartTime));
    char endTimestamp[32];
    strftime(endTimestamp, sizeof(endTimestamp), "%FT%H:%M:%SZ", gmtime(&currentTime));

    char jsonBuf[256];
    snprintf(
        jsonBuf,
        sizeof(jsonBuf),
        "{\"deltaEnergy\":%0.1f,\"start\":\"%s\",\"end\":\"%s\",\"power\":%0.1f,\"energy\":%0.1f}",
        energyDelta,
        startTimestamp,
        endTimestamp,
        power,
        energyTotal);
    TRACE("JSON: %s\n", jsonBuf);

	iot_cap_val_t value;
	value.type = IOT_CAP_VAL_TYPE_JSON_OBJECT;
	value.json_object = jsonBuf;
	IOT_EVENT *attr = st_cap_create_attr(iotPowerConsumptionReportCapHandle, "powerConsumption", &value, nullptr, nullptr);
	if (attr != nullptr)
    {
		int sequenceNr = st_cap_send_attr(&attr, 1);
        TRACE("Sequence #%d\n", sequenceNr);
		st_cap_free_attr(attr);
	}
}

static void onSmartThingsPowerMeterInit(IOT_CAP_HANDLE *handle, void *usr_data)
{
    TRACE("onSmartThingsPowerMeterInit\n");
    powerChangeTime = TimeServer.getCurrentTime() + 60;
    sendPower();    
}

static void onSmartThingsEnergyMeterInit(IOT_CAP_HANDLE *handle, void *usr_data)
{
    TRACE("onSmartThingsEnergyMeterInit\n");
    sendEnergy();    
}

static void onSmartThingsPowerConsumptionReportInit(IOT_CAP_HANDLE *handle, void *usr_data)
{
    TRACE("onSmartThingsPowerConsumptionReportInit\n");
    powerConsumptionReportStartTime = TimeServer.getCurrentTime();
    sendPowerConsumption();
}


size_t readFromFile(const char* fileName, uint8_t* buffer, size_t bufferSize)
{
    Tracer tracer("readFromFile", fileName);

    File file = SPIFFS.open(fileName);
    size_t bytesRead = file.readBytes((char*)buffer, bufferSize);
    file.close();

    TRACE("Read %u bytes.\n", bytesRead);
    return bytesRead;
}


void initializeSmartThings()
{
    Tracer tracer("initializeSmartThings");

    uint8_t onboardingConfig[1024];
    int onboardingConfigLength = readFromFile("/onboarding_config.json", onboardingConfig, sizeof(onboardingConfig));

    uint8_t deviceInfo[256];
    int deviceInfoLength = readFromFile("/device_info.json", deviceInfo, sizeof(deviceInfo));

    iotContext = st_conn_init(onboardingConfig, onboardingConfigLength, deviceInfo, deviceInfoLength);
    if (iotContext != nullptr)
    {
        int err = st_conn_set_noti_cb(iotContext, onSmartThingsNotification, nullptr);
        TRACE("st_conn_set_noti_cb returned %d\n", err);

        iotPowerMeterCapHandle = st_cap_handle_init(iotContext, "main", "powerMeter", onSmartThingsPowerMeterInit, nullptr);
        iotEnergyMeterCapHandle = st_cap_handle_init(iotContext, "main", "energyMeter", onSmartThingsEnergyMeterInit, nullptr);
        iotPowerConsumptionReportCapHandle = st_cap_handle_init(iotContext, "main", "powerConsumptionReport", onSmartThingsPowerConsumptionReportInit, nullptr);

        TRACE("SmartThings initialized. Connecting...\n");
        err = st_conn_start(iotContext, onSmartThingsStatusUpdate, IOT_STATUS_ALL, nullptr, nullptr);
        if (err) 
        {
            WiFiSM.logEvent("Error connecting to SmartThings: %d", err);
        }
    }
    else
        WiFiSM.logEvent("Unable to initialize SmartThings");
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
    //WiFiSM.scanAccessPoints();
    //WiFiSM.begin(PersistentData.wifiSSID, PersistentData.wifiKey, PersistentData.hostName);

    initializeSmartThings();

    Tracer::traceFreeHeap();

    BuiltinLED.setOn(false);    
}


// Called repeatedly
void loop() 
{
    currentTime = TimeServer.getCurrentTime();

    if (Serial.available())
        handleSerialRequest();

    // Let WiFi State Machine handle initialization and web requests
    // This also calls the onXXX methods below
    // WiFiSM.run();

    if (powerChangeTime > 0 && currentTime >= powerChangeTime)
    {
        powerChangeTime += 60;

        power += 0.1;
        sendPower();

        energyTotal += 1.1;
        sendEnergy();

        energyDelta += 1.1;
        if (currentTime >= powerConsumptionReportStartTime + 300)
        {
            powerConsumptionReportStartTime = currentTime;
            energyDelta = 0;
        }
        sendPowerConsumption();
    }

}
