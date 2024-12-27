#include <Arduino.h>
#include <deque>
#include <bme68xLibrary.h>
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
#include "FanLogEntry.h"
#include "FanControl.h"
#include "MovingAverage.h"

enum FileId
{
    Logo,
    Styles,
    HomeIcon,
    GraphIcon,
    LogFileIcon,
    SettingsIcon,
    UploadIcon,
    ToolIcon,
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
    "Upload.svg",
    "Tool.svg"
};

SimpleLED BuiltinLED(LED_BUILTIN, true);
ESPWebServer WebServer(80); // Default HTTP port
WiFiNTP TimeServer;
WiFiFTPClient FTPClient(FTP_TIMEOUT_MS);
StringBuilder HttpResponse(HTTP_RESPONSE_BUFFER_SIZE);
HtmlWriter Html(HttpResponse, Files[Logo], Files[Styles]);
StringLog EventLog(MAX_EVENT_LOG_SIZE, 128);
WiFiStateMachine WiFiSM(BuiltinLED, TimeServer, WebServer, EventLog);
Navigation Nav;
FanControlClass FanControl(FAN_DAC_PIN, FAN_ADC_PIN);
MovingAverage HumidityBaseline(100); // 100 points; 5 minutes @ 3s sample rate

StaticLog<FanLogEntry> FanLog(FAN_LOG_SIZE);
FanLogEntry NewFanLogEntry;
FanLogEntry* lastFanLogEntryPtr = nullptr;
int fanLogAggregations = 0;
int fanLogEntriesToSync = 0;


time_t currentTime = 0;
time_t calibrateUntil = 0;
time_t iaqPollTime = 0;
time_t syncFTPTime = 0;
time_t lastFTPSyncTime = 0;

Bme68x IAQSensor;
bme68xData IAQData;
int8_t lastBmeStatus = BME68X_OK;


bool checkIAQStatus()
{
    bool result = true;

    int8_t bmeStatus = IAQSensor.checkStatus(); 
    if (bmeStatus != BME68X_OK) 
    {
        if (bmeStatus != lastBmeStatus)
            WiFiSM.logEvent(F("BME: %s"), IAQSensor.statusString().c_str());
        result = false;
    }

    lastBmeStatus = bmeStatus;
    return result;
}


bool initializeIAQSensor()
{
    Tracer tracer("initializeIAQSensor");

    if (!Wire.begin(BME_SDA_PIN, BME_SCL_PIN, 400000))
    {
        WiFiSM.logEvent("Unable to initialize I2C");
        return false;
    }
    
    IAQSensor.begin(BME68X_I2C_ADDR_HIGH, Wire);
    if (!checkIAQStatus()) 
    {
        WiFiSM.logEvent("Unable to initialize IAQ sensor");
        return false;
    }

    IAQSensor.setTPH(BME68X_OS_4X, BME68X_OS_4X, BME68X_OS_8X);

    TRACE(
        "BME measure duration: %0.1f us\n",
        float(IAQSensor.getMeasDur(BME68X_FORCED_MODE)) / 1000);

    const bme68xHeatrConf& heaterConf = IAQSensor.getHeaterConfiguration();
    TRACE(
        "BME heater: enable=%d, temp=%d, duration=%d\n",
        heaterConf.enable,
        heaterConf.heatr_temp,
        heaterConf.heatr_dur);

    WiFiSM.logEvent("BME sensor ID: %u", IAQSensor.getUniqueId());
    return true;
}


void updateIAQSensorValues()
{
    HumidityBaseline.addValue(roundf(IAQData.humidity * 10));
    float avgHumidity = HumidityBaseline.getAverage() / 10;

    if (calibrateUntil == 0)
        FanControl.setHumidity(IAQData.humidity, avgHumidity);
    else if (currentTime >= calibrateUntil)
        calibrateUntil = 0;

    NewFanLogEntry.time = currentTime;
    NewFanLogEntry.humidity = IAQData.humidity;
    NewFanLogEntry.humidityBaselineDelta = IAQData.humidity - avgHumidity;
    NewFanLogEntry.temperature = IAQData.temperature;
    NewFanLogEntry.pressure = IAQData.pressure / 100; // hPa
    NewFanLogEntry.fanLevel = FanControl.getLevel();

    if ((lastFanLogEntryPtr == nullptr) || !NewFanLogEntry.equals(lastFanLogEntryPtr))
    {
        lastFanLogEntryPtr = FanLog.add(&NewFanLogEntry);
        fanLogEntriesToSync = std::min(fanLogEntriesToSync + 1, FAN_LOG_SIZE);
        if ((fanLogEntriesToSync == PersistentData.ftpSyncEntries) && PersistentData.isFTPEnabled())
            syncFTPTime = currentTime;
    }
}


void handleHttpLevelRequest()
{
    Tracer tracer("handleHttpLevelRequest");

    if (WebServer.hasArg("set"))
    {
        String set = WebServer.arg("set");
        if (set == "max")
            FanControl.setMaxLevel();
        else
        {
            int level = set.toInt();
            if ((level > 0) && (level <= 100))
            {
                uint32_t duration = WebServer.hasArg("duration") ? WebServer.arg("duration").toInt() : 0;
                FanControl.setLevel(level, duration);
            }
            else
                TRACE("Invalid level: '%s'\n", set.c_str());
        }
    }

    HttpResponse.clear();
    HttpResponse.printf(F("{ \"level\": %d"), FanControl.getLevel());
    if (FanControl.keepLevelUntil() != 0)
        HttpResponse.printf(F(", \"keepUntil\": \"%s\""), formatTime("%T", FanControl.keepLevelUntil()));
    HttpResponse.println(" }");

    WebServer.send(200, ContentTypeJson, HttpResponse.c_str());
}


void writeFanLogCsv(Print& output, int entriesFromEnd)
{
    FanLogEntry* logEntryPtr = (entriesFromEnd == 0)
        ? FanLog.getFirstEntry()
        : FanLog.getEntryFromEnd(entriesFromEnd);

    while (logEntryPtr != nullptr)
    {
        logEntryPtr->writeCsv(output);
        logEntryPtr = FanLog.getNextEntry();
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
    snprintf(filename, sizeof(filename), "%s.csv", PersistentData.hostName);
    bool success = false;
    WiFiClient& dataClient = FTPClient.append(filename);
    if (dataClient.connected())
    {
        if (fanLogEntriesToSync != 0)
        {
            writeFanLogCsv(dataClient, fanLogEntriesToSync);
            fanLogEntriesToSync = 0;
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
    iaqPollTime = currentTime;
    initializeIAQSensor();
}


void onWiFiInitialized()
{
    if (currentTime >= iaqPollTime)
    {
        iaqPollTime = currentTime + IAQ_POLL_INTERVAL;
        BuiltinLED.setOn();
        if (checkIAQStatus())
        {
            IAQSensor.setOpMode(BME68X_FORCED_MODE);
            delay((IAQSensor.getMeasDur() / 1000) + 1);
            if (IAQSensor.fetchData())
            {
                IAQSensor.getData(IAQData);
                updateIAQSensorValues();
            }
        }
        BuiltinLED.setOff();
    }

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
            syncFTPTime = currentTime + FTP_RETRY_INTERVAL;
        }
    }
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
    HttpResponse.println("Time;Humidity (%);Delta (%);T (°C);P (hPa);Fan (%)");
    Html.writePreEnd();

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


void handleHttpFanLogRequest()
{
    Tracer tracer("handleHttpFanLogRequest");

    // Use chunked response
    WebServer.setContentLength(CONTENT_LENGTH_UNKNOWN);
    WebServer.send(200, ContentTypeHtml, "");

    Html.writeHeader("Fan Log", Nav);

    int currentPage = WebServer.hasArg("page") ? WebServer.arg("page").toInt() : 0;
    int totalPages = (FanLog.count() > 0) ? ((FanLog.count() - 1)/ FAN_LOG_PAGE_SIZE) + 1 : 1;
    Html.writePager(totalPages, currentPage);

    Html.writeTableStart();
    Html.writeRowStart();
    Html.writeHeaderCell("Time");
    Html.writeHeaderCell("Humidity (%)");
    Html.writeHeaderCell("Delta (%)");
    Html.writeHeaderCell("T (°C)");
    Html.writeHeaderCell("P (hPa)");
    Html.writeHeaderCell("Fan (%)");
    Html.writeRowEnd();

    FanLogEntry* logEntryPtr = FanLog.getFirstEntry();
    for (int i = 0; (i < (currentPage * FAN_LOG_PAGE_SIZE)) && (logEntryPtr != nullptr); i++)
        logEntryPtr = FanLog.getNextEntry();

    for (int i = 0; (i < FAN_LOG_PAGE_SIZE) && (logEntryPtr != nullptr); i++)
    {
        logEntryPtr->writeHtml(Html);
        if (HttpResponse.length() >= HTTP_CHUNK_SIZE)
        {
            WebServer.sendContent(HttpResponse.c_str(), HttpResponse.length());
            HttpResponse.clear();
        }
        logEntryPtr = FanLog.getNextEntry();
    }

    Html.writeTableEnd();
    Html.writeFooter();

    WebServer.sendContent(HttpResponse.c_str(), HttpResponse.length());
    WebServer.sendContent("");
}


void handleHttpCalibrateFormRequest()
{
    Tracer tracer("handleHttpCalibrateFormRequest");

    uint8_t level = WebServer.hasArg("level") ? WebServer.arg("level").toInt() : 50;

    calibrateUntil = currentTime + CALIBRATE_TIME + SECONDS_PER_MINUTE;
    FanControl.setLevel(level, CALIBRATE_TIME);
    delay(500);
    float adcVoltage = FanControl.getVoltage();

    Html.writeHeader("Calibrate", Nav);

    Html.writeFormStart("/calibrate", "grid");
    Html.writeNumberBox("level", "Fan level (%)", level, 0, 100);
    Html.writeNumberBox("voltage", "Voltage", adcVoltage, 1, 12, 2);
    Html.writeSubmitButton("Calibrate");
    Html.writeFormEnd();

    Html.writeFooter();

    WebServer.send(200, ContentTypeHtml, HttpResponse.c_str());
}


void handleHttpCalibrateFormPost()
{
    Tracer tracer("handleHttpCalibrateFormPost");

    float measuredVoltage = WebServer.arg("voltage").toFloat();
    FanControl.calibrate(measuredVoltage, PersistentData.dacScale, PersistentData.adcScale);
    PersistentData.validate();
    PersistentData.writeToEEPROM();

    handleHttpCalibrateFormRequest();
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

    FanControl.maxLevel = PersistentData.maxLevel;
    FanControl.maxLevelDuration = PersistentData.maxLevelDuration;
    FanControl.humidityThreshold = PersistentData.humidityThreshold;

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
    
    Html.writeHeader("Home", Nav);

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
    Html.writeRow("Sync entries", "%d / %d", fanLogEntriesToSync, PersistentData.ftpSyncEntries);
    Html.writeTableEnd();
    Html.writeSectionEnd();

    Html.writeSectionStart("Sensor values");
    Html.writeTableStart();
    Html.writeRow("Temperature", "%0.1f °C", IAQData.temperature);
    Html.writeRow("Pressure", "%0.1f hPa", IAQData.pressure / 100);
    Html.writeRow("Humidity", "%0.1f %%", IAQData.humidity);
    Html.writeTableEnd();
    Html.writeSectionEnd();

    float avgHumidity = HumidityBaseline.getAverage() / 10;
    Html.writeSectionStart("Humidity baseline");
    Html.writeTableStart();
    Html.writeRow("Minimum", "%0.1f %%", float(HumidityBaseline.getMinimum()) / 10);
    Html.writeRow("Maximum", "%0.1f %%", float(HumidityBaseline.getMaximum()) / 10);
    Html.writeRow("Slope", "%0.2f %%/min", HumidityBaseline.getSlope() * IAQ_SAMPLES_PER_MINUTE / 10);
    Html.writeRow("Average", "%0.1f %%", avgHumidity);
    Html.writeRow("Delta", "%0.1f %%", IAQData.humidity - avgHumidity);
    Html.writeTableEnd();
    Html.writeSectionEnd();

    Html.writeSectionStart("Fan control");
    Html.writeTableStart();
    Html.writeRow("Level", "%d %%", FanControl.getLevel());
    Html.writeRow("Voltage", "%0.1f V", FanControl.getVoltage());
    if (FanControl.keepLevelUntil() != 0)
        Html.writeRow("Keep until", "%s", formatTime("%T", FanControl.keepLevelUntil()));
    Html.writeTableEnd();
    Html.writeSectionEnd();

    Html.writeDivEnd();
    Html.writeFooter();

    WebServer.send(200, ContentTypeHtml, HttpResponse.c_str());
}


void handleSerialRequest()
{
    Tracer tracer("handleSerialRequest");

    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    Serial.println(cmd);

    if (cmd.startsWith("testH="))
    {
        String humidity = cmd.substring(6);
        TRACE("Humidity: '%s'\n", humidity.c_str());
        IAQData.humidity = humidity.toFloat();
        IAQData.temperature = 20.11;
        IAQData.pressure = 101311;
        updateIAQSensorValues();
    }
}


// Boot code
void setup() 
{
#ifdef DEBUG_ESP_PORT
    DEBUG_ESP_PORT.begin(DEBUG_BAUDRATE);
    DEBUG_ESP_PORT.setDebugOutput(true);
    DEBUG_ESP_PORT.println();
    Tracer::traceTo(DEBUG_ESP_PORT);
    Tracer::traceFreeHeap();
#endif

    BuiltinLED.begin();
    EventLog.begin();

    PersistentData.begin();
    TimeServer.begin(PersistentData.ntpServer);
    Html.setTitlePrefix(PersistentData.hostName);
    
    FanControl.maxLevel = PersistentData.maxLevel;
    FanControl.maxLevelDuration = PersistentData.maxLevelDuration;
    FanControl.humidityThreshold = PersistentData.humidityThreshold;
    if (!FanControl.begin(PersistentData.dacScale, PersistentData.adcScale))
        WiFiSM.logEvent("Fan control initialization failed");

    Nav.menuItems =
    {
        MenuItem
        {
            .icon = Files[HomeIcon],
            .label = "Home",
            .handler = handleHttpRootRequest            
        },
        MenuItem
        {
            .icon = Files[LogFileIcon],
            .label = "Event log",
            .urlPath ="events",
            .handler = handleHttpEventLogRequest
        },
        MenuItem
        {
            .icon = Files[GraphIcon],
            .label = "Fan log",
            .urlPath ="fanlog",
            .handler = handleHttpFanLogRequest
        },
        MenuItem
        {
            .icon = Files[UploadIcon],
            .label = "FTP Sync",
            .urlPath = "sync",
            .handler= handleHttpSyncFTPRequest
        },
        MenuItem
        {
            .icon = Files[ToolIcon],
            .label = "Calibrate",
            .urlPath ="calibrate",
            .handler = handleHttpCalibrateFormRequest,
            .postHandler = handleHttpCalibrateFormPost
        },
        MenuItem
        {
            .icon = Files[SettingsIcon],
            .label = "Settings",
            .urlPath ="config",
            .handler = handleHttpConfigFormRequest,
            .postHandler = handleHttpConfigFormPost
        },
    };
    Nav.registerHttpHandlers(WebServer);

    WebServer.on("/level", handleHttpLevelRequest);

    WiFiSM.registerStaticFiles(Files, _LastFile);
    WiFiSM.on(WiFiInitState::TimeServerSynced, onTimeServerSynced);
    WiFiSM.on(WiFiInitState::Initialized, onWiFiInitialized);
    WiFiSM.scanAccessPoints();
    WiFiSM.begin(PersistentData.wifiSSID, PersistentData.wifiKey, PersistentData.hostName);

    Tracer::traceFreeHeap();

    BuiltinLED.setOff();    
}


// Called repeatedly
void loop() 
{
    currentTime = WiFiSM.getCurrentTime();

    WiFiSM.run();

    if (Serial.available())
        handleSerialRequest();
}
