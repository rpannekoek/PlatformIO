#include <Arduino.h>
#include <ESPWiFi.h>
#include <ESPWebServer.h>
#include <ESPFileSystem.h>
#include <WiFiStateMachine.h>
#include <WiFiNTP.h>
#include <WiFiFTP.h>
#include <TimeUtils.h>
#include <Tracer.h>
#include <StringBuilder.h>
#include <HtmlWriter.h>
#include <Navigation.h>
#include <LED.h>
#include <Log.h>
#include <FlowSensor.h>
#include <EnergyMeter.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <Ticker.h>
#include "PersistentData.h"
#include "MonitoredTopics.h"
#include "DayStatsEntry.h"

constexpr float SPECIFIC_HEAT_CAP_H2O = 4.186;
constexpr uint16_t HTTP_RESPONSE_BUFFER_SIZE = 8 * 1024;
constexpr int EVENT_LOG_LENGTH = 50;
constexpr int FTP_TIMEOUT_MS = 2000;
constexpr uint32_t FTP_RETRY_INTERVAL = 15 * SECONDS_PER_MINUTE;
constexpr uint32_t HEAT_LOG_INTERVAL = 30 * SECONDS_PER_MINUTE;
constexpr float DS18_INIT_VALUE_C = 85.0;

constexpr uint8_t MAX_TEMP_VALVE_PIN = D8;

const char* ContentTypeHtml = "text/html;charset=UTF-8";
const char* ContentTypeJson = "application/json";
const char* ButtonClass = "button";

enum FileId
{
    Logo,
    Styles,
    CalibrateIcon,
    GraphIcon,
    HomeIcon,
    LogFileIcon,
    SettingsIcon,
    UploadIcon,
    _LastFile
};

const char* Files[] PROGMEM =
{
    "Logo.png",
    "styles.css",
    "Calibrate.svg",
    "Graph.svg",
    "Home.svg",
    "LogFile.svg",
    "Settings.svg",
    "Upload.svg"
};

ESPWebServer WebServer(80); // Default HTTP port
WiFiNTP TimeServer;
WiFiFTPClient FTPClient(FTP_TIMEOUT_MS);
StringBuilder HttpResponse(HTTP_RESPONSE_BUFFER_SIZE);
HtmlWriter Html(HttpResponse, Files[Logo], Files[Styles]);
StringLog EventLog(EVENT_LOG_LENGTH, 96);
StaticLog<HeatLogEntry> HeatLog(24 * 2); // 24 hrs
StaticLog<DayStatsEntry> DayStats(31); // 31 days
SimpleLED BuiltinLED(LED_BUILTIN, true);
WiFiStateMachine WiFiSM(BuiltinLED, TimeServer, WebServer, EventLog);
Navigation Nav;

OneWire OneWireBus(D7);
DallasTemperature TempSensors(&OneWireBus);
Ticker TempTicker;
FlowSensor Flow_Sensor(D6);
EnergyMeter Energy_Meter(D2);

time_t currentTime = 0;
time_t heatLogTime = 0;
time_t syncFTPTime = 0;
time_t lastFTPSyncTime = 0;

HeatLogEntry* lastHeatLogEntryPtr = nullptr;
DayStatsEntry* lastDayStatsEntryPtr = nullptr;

bool newSensorFound = false;
bool maxTempValveActivated = false;
bool maxTempValveOverride = false;

float currentValues[NUMBER_OF_TOPICS];
bool testOverrides[NUMBER_OF_TOPICS];


void logSensorInfo(TopicId sensorId)
{
    MonitoredTopic topic = MonitoredTopics[sensorId];
    DeviceAddress& addr = PersistentData.tempSensorAddress[sensorId];

    char message[64];
    if (TempSensors.isConnected(addr))
    {
        snprintf(
            message,
            sizeof(message),
            "%s sensor: %02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X. %d bits. Offset: %0.2f",
            topic.label,
            addr[0], addr[1], addr[2], addr[3], addr[4], addr[5], addr[6], addr[7],
            TempSensors.getResolution(addr),
            PersistentData.tempSensorOffset[sensorId]
            );
    }
    else
    {
        snprintf(
            message,
            sizeof(message),
            "ERROR: %s sensor is not connected.",
            topic.label
            );
    }

    WiFiSM.logEvent(message);
}


void setMaxTempValve(bool activated)
{
    maxTempValveActivated = activated;
    digitalWrite(MAX_TEMP_VALVE_PIN, maxTempValveActivated ? 1 : 0);
    WiFiSM.logEvent(F("Max temperature valve set %s"), activated ? "on" : "off");
}


void initTempSensors()
{
    Tracer tracer(F("initTempSensors"));

    TRACE(F("Found %d OneWire devices.\n"), TempSensors.getDeviceCount());
    TRACE(F("Found %d temperature sensors.\n"), TempSensors.getDS18Count());

    for (int i = 0; i < 3; i++)
    {
        MonitoredTopic topic = MonitoredTopics[i];

        if ((TempSensors.getDS18Count() > i) && !TempSensors.validFamily(PersistentData.tempSensorAddress[i]))
        {
            newSensorFound = TempSensors.getAddress(PersistentData.tempSensorAddress[i], i);
            if (!newSensorFound)
            {
                WiFiSM.logEvent(F("ERROR: Unable to obtain sensor address for %s"), topic.label);
            }
        }
    }

    if (newSensorFound)
        PersistentData.writeToEEPROM();

    TempSensors.setResolution(12);
}


void readTempSensors()
{
    if (TempSensors.isConversionComplete())
    {
        BuiltinLED.setOn(true);
        for (int i = 0; i < 3; i++)
        {
            float tMeasured = TempSensors.getTempC(PersistentData.tempSensorAddress[i]);
            if (tMeasured != DEVICE_DISCONNECTED_C)
            {
                if ((tMeasured == DS18_INIT_VALUE_C) && (abs(tMeasured - currentValues[i]) > 5.0F))
                    WiFiSM.logEvent(F("Invalid %s sensor value"), MonitoredTopics[i].label);
                else
                    currentValues[i] = tMeasured + PersistentData.tempSensorOffset[i];
            }
        }
        TempSensors.requestTemperatures();
        BuiltinLED.setOn(false);
    }

    if (!maxTempValveOverride && (PersistentData.tBufferMax != 0))
    {
        if ((currentValues[TopicId::TBuffer] > PersistentData.tBufferMax) && !maxTempValveActivated)
            setMaxTempValve(true);

        if ((currentValues[TopicId::TBuffer] < (PersistentData.tBufferMax - PersistentData.tBufferMaxDelta)) && maxTempValveActivated)
            setMaxTempValve(false);
    }
}

void newHeatLogEntry()
{
    HeatLogEntry newHeatLogEntry;
    newHeatLogEntry.time = currentTime - (currentTime % HEAT_LOG_INTERVAL);
    lastHeatLogEntryPtr = HeatLog.add(&newHeatLogEntry);
}


// Create new energy log entry (starting 00:00 current day)
void newDayStatsEntry()
{
    DayStatsEntry newEntry;
    newEntry.time = currentTime - (currentTime % SECONDS_PER_DAY);
    lastDayStatsEntryPtr = DayStats.add(&newEntry);
}


bool trySyncFTP(Print* printTo)
{
    Tracer tracer(F("trySyncFTP"));

    char filename[64];
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
        if (DayStats.count() > 1)
        {
            DayStatsEntry& yesterdayStats = *DayStats.at(-2);
            dataClient.printf(
                "%s;%0.1f;%0.1f\r\n",
                formatTime("%F", yesterdayStats.time),
                yesterdayStats.energyIn,
                yesterdayStats.energyOut
                );
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


float calcPower(float flowRate, float deltaT)
{
    return SPECIFIC_HEAT_CAP_H2O * (flowRate / 60) * deltaT;
}


void calculateValues()
{
    if (!testOverrides[TopicId::DeltaT])
        currentValues[TopicId::DeltaT] = currentValues[TopicId::TInput] - currentValues[TopicId::TOutput];

    if (!testOverrides[TopicId::FlowRate])
        currentValues[TopicId::FlowRate] = Flow_Sensor.getFlowRate();

    if (!testOverrides[TopicId::POut])
        currentValues[TopicId::POut] = calcPower(
            currentValues[TopicId::FlowRate],
            currentValues[TopicId::TInput] - currentValues[TopicId::TOutput]);

    if (!testOverrides[TopicId::PIn])
        currentValues[TopicId::PIn] = Energy_Meter.getPower() / 1000;
}


void updateHeatLog()
{
    if (currentTime >= lastHeatLogEntryPtr->time + HEAT_LOG_INTERVAL)
    {
        newHeatLogEntry();
    }

    uint32_t valveSeconds = maxTempValveActivated ? 1 : 0;
    lastHeatLogEntryPtr->update(currentValues, valveSeconds);
}


void updateDayStats()
{
    if (currentTime >= lastDayStatsEntryPtr->time + SECONDS_PER_DAY)
    {
        Energy_Meter.resetEnergy();
        newDayStatsEntry();
        if (PersistentData.isFTPEnabled())
            syncFTPTime = currentTime;
    }

    if (maxTempValveActivated) lastDayStatsEntryPtr->valveActivatedSeconds++;
    lastDayStatsEntryPtr->energyOut += currentValues[TopicId::POut] / 3600;
    lastDayStatsEntryPtr->energyIn = Energy_Meter.getEnergy();
}


void test(String message)
{
    Tracer tracer(F("test"), message.c_str());

    if (message.startsWith("L"))
    {
        for (int i = 0; i < EVENT_LOG_LENGTH; i++)
        {
            WiFiSM.logEvent(F("Test event to fill the event log"));
            yield();
        }
    }
    else if (message.startsWith("H"))
    {
        float testValues[NUMBER_OF_TOPICS];
        for (int i = 0; i < 48; i++)
        {
            float tInput = (i % 40) + 25;
            testValues[TopicId::TInput] = tInput;
            testValues[TopicId::TOutput] = tInput - 10;
            testValues[TopicId::TBuffer] = tInput - 5;
            testValues[TopicId::DeltaT] = 10;
            testValues[TopicId::FlowRate] = tInput / 2;
            testValues[TopicId::POut] = calcPower(testValues[TopicId::FlowRate], testValues[TopicId::DeltaT]);
            testValues[TopicId::PIn] = testValues[TopicId::POut] / 4;
            uint32_t valveSeconds = i * 30;
            lastHeatLogEntryPtr->time = currentTime + i * HEAT_LOG_INTERVAL; 
            lastHeatLogEntryPtr->update(testValues, valveSeconds);
            newHeatLogEntry();
        }
    }
    else if (message.startsWith("D"))
    {
        for (int i = 0; i < 31; i++)
        {
            lastDayStatsEntryPtr->energyIn = float(i) / 10;
            lastDayStatsEntryPtr->energyOut = float(i) / 2.5;
            lastDayStatsEntryPtr->valveActivatedSeconds = i * 30;
            lastDayStatsEntryPtr->time = currentTime + i * SECONDS_PER_DAY;
            newDayStatsEntry();
        }
    }
    else if (message.startsWith("T"))
    {
        int eqaulSignIndex = message.indexOf('=');
        if (eqaulSignIndex > 1)
        {
            String topicName = message.substring(1, eqaulSignIndex);
            topicName.trim();

            int topicId = -1;
            for (int i = 0; i < NUMBER_OF_TOPICS; i++)
            {
                if (topicName == MonitoredTopics[i].label)
                {
                    topicId = i;
                    break;
                }
            }
            if (topicId == -1)
                TRACE(F("Unknown topic: '%s'\n"), topicName.c_str());                
            else
            {
                float topicValue = message.substring(eqaulSignIndex + 1).toFloat();
                TRACE(F("Settings topic '%s' (#%d) to %0.1f\n"), topicName.c_str(), topicId, topicValue);
                currentValues[topicId] = topicValue;
                testOverrides[topicId] = true;
            }
        }
        else
            TRACE(F("Unexpected test command:'%s'\n"), message.c_str());
    }
}


void writeJsonFloat(String name, float value)
{
    HttpResponse.printf(F("\"%s\": %0.1f,"), name.c_str(), value);
}


void handleHttpJsonRequest()
{
    Tracer tracer(F("handleHttpJsonRequest"));

    HttpResponse.clear();
    HttpResponse.print(F("{ "));

    for (int i = 0; i < NUMBER_OF_TOPICS; i++)
    {
        MonitoredTopic topic = MonitoredTopics[i];
        float topicValue = currentValues[topic.id];
        HttpResponse.printf(F("\"%s\": %s, "), topic.label, topic.formatValue(topicValue, false));
    }

    HttpResponse.printf(F(" \"Valve\": %s }"), maxTempValveActivated ? "true" : "false");

    WebServer.send(200, ContentTypeJson, HttpResponse);
}


void handleHttpFtpSyncRequest()
{
    Tracer tracer(F("handleHttpFtpSyncRequest"));
    ChunkedResponse response(HttpResponse, WebServer, ContentTypeHtml);

    Html.writeHeader(F("FTP Sync"), Nav);

    HttpResponse.println(F("<pre>"));
    bool success = trySyncFTP(&HttpResponse); 
    HttpResponse.println(F("</pre>"));

    if (success)
    {
        Html.writeParagraph(F("Success!"));
        syncFTPTime = 0; // Cancel scheduled sync (if any)
    }
    else
        Html.writeParagraph(F("Failed: %s"), FTPClient.getLastError());
 
    Html.writeFooter();
}


void writeMinMaxAvgHeader(int repeat)
{
    Html.writeRowStart();
    for (int i = 0; i < repeat; i++)
    {
        Html.writeHeaderCell(F("Min"));
        Html.writeHeaderCell(F("Max"));
        Html.writeHeaderCell(F("Avg"));
    }
    Html.writeRowEnd();
}


void handleHttpHeatLogRequest()
{
    Tracer tracer(F("handleHttpHeatLogRequest"));
    ChunkedResponse response(HttpResponse, WebServer, ContentTypeHtml);

    TopicId showTopicsIds[] = { TopicId::DeltaT, TopicId::FlowRate, TopicId::POut, TopicId::PIn };

    float maxPower = 0.01F; // Prevent division by zero
    for (HeatLogEntry& logEntry : HeatLog)
    {
        maxPower = std::max(maxPower, logEntry.getAverage(TopicId::PIn));
        maxPower = std::max(maxPower, logEntry.getAverage(TopicId::POut));
    }
    TRACE(F("maxPower: %0.1f\n"), maxPower);

    Html.writeHeader(F("Heat log"), Nav);

    HttpResponse.printf(F("<p>Max: %0.2f kW</p>\r\n"), maxPower);

    Html.writeTableStart();

    Html.writeRowStart();
    Html.writeHeaderCell(F("Time"), 0, 2);
    Html.writeHeaderCell(F("ΔT (°C)"), 3);
    Html.writeHeaderCell(F("Flow (l/min)"), 3);
    Html.writeHeaderCell(F("P<sub>out</sub> (kW)"), 3);
    Html.writeHeaderCell(F("P<sub>in</sub> (kW)"), 3);
    Html.writeRowEnd();

    writeMinMaxAvgHeader(4);

    for (HeatLogEntry& logEntry : HeatLog)
    {
        float avgPIn = logEntry.getAverage(TopicId::PIn);
        float avgPOut = logEntry.getAverage(TopicId::POut);

        Html.writeCell(formatTime("%H:%M", logEntry.time));
        for (TopicId topicId : showTopicsIds)
        {
            TopicStats topicStats = logEntry.topicStats[topicId];
            Html.writeCell(topicStats.min);
            Html.writeCell(topicStats.max);
            Html.writeCell(logEntry.getAverage(topicId), F("%0.2f"));
        }
        Html.writeGraphCell(
            avgPIn,
            avgPOut,
            0,
            maxPower,
            F("pInBar"),
            F("powerBar"),
            false
            );
        Html.writeRowEnd();
    }
    Html.writeTableEnd();
    Html.writeFooter();
}


void handleHttpTempLogRequest()
{
    Tracer tracer(F("handleHttpTempLogRequest"));
    ChunkedResponse response(HttpResponse, WebServer, ContentTypeHtml);

    TopicId showTopicsIds[] = { TopicId::TInput, TopicId::TOutput };

    Html.writeHeader(F("Temperature log"), Nav);

    float tMin = 20.0F;
    float tMax = 60.0F;
    HttpResponse.printf(F("<p>Min: %0.1f °C. Max: %0.1f °C.</p>\r\n"), tMin, tMax);

    Html.writeTableStart();

    Html.writeRowStart();
    Html.writeHeaderCell(F("Time"), 0, 2);
    Html.writeHeaderCell(F("T<sub>in</sub> (°C)"), 3);
    Html.writeHeaderCell(F("T<sub>out</sub> (°C)"), 3);
    Html.writeRowEnd();

    writeMinMaxAvgHeader(2);

    for (HeatLogEntry& logEntry : HeatLog)
    {
        float avgTInput = logEntry.getAverage(TopicId::TInput);
        float avgTOutput = logEntry.getAverage(TopicId::TOutput);

        Html.writeRowStart();
        Html.writeCell(formatTime("%H:%M", logEntry.time));
        for (TopicId topicId : showTopicsIds)
        {
            TopicStats topicStats = logEntry.topicStats[topicId];
            Html.writeCell(topicStats.min);
            Html.writeCell(topicStats.max);
            Html.writeCell(logEntry.getAverage(topicId));
        }
        Html.writeGraphCell(
            avgTOutput,
            avgTInput,
            tMin,
            tMax,
            F("tOutBar"),
            F("waterBar"),
            false
            );
        Html.writeRowEnd();
    }
    Html.writeTableEnd();
    Html.writeFooter();
}


void handleHttpBufferLogRequest()
{
    Tracer tracer(F("handleHttpBufferLogRequest"));
    ChunkedResponse response(HttpResponse, WebServer, ContentTypeHtml);

    // Auto-ranging: determine min & max buffer temp
    float tMin = 666;
    float tMax = 0;
    for (HeatLogEntry& logEntry : HeatLog)
    {
        float avg = logEntry.getAverage(TopicId::TBuffer);
        tMin = std::min(tMin, avg);
        tMax = std::max(tMax, avg);
    }
    tMax = std::max(tMax, tMin + 1); // Prevent division by zero

    Html.writeHeader(F("Buffer log"), Nav);
    
    HttpResponse.printf(F("<p>Min: %0.1f °C. Max: %0.1f °C.</p>\r\n"), tMin, tMax);

    Html.writeTableStart();
    Html.writeRowStart();
    Html.writeHeaderCell(F("Time"), 0, 2);
    Html.writeHeaderCell(F("Valve on"), 0, 2);
    Html.writeHeaderCell(F("T<sub>buffer</sub> (°C)"), 3);
    Html.writeRowEnd();
    writeMinMaxAvgHeader(1);

    for (HeatLogEntry& logEntry : HeatLog)
    {
        TopicStats topicStats = logEntry.topicStats[TopicId::TBuffer];
        float avgTBuffer = logEntry.getAverage(TopicId::TBuffer);

        Html.writeRowStart();
        Html.writeCell(formatTime("%H:%M", logEntry.time));
        Html.writeCell(formatTimeSpan(logEntry.valveActivatedSeconds));
        Html.writeCell(topicStats.min);
        Html.writeCell(topicStats.max);
        Html.writeCell(avgTBuffer);
        Html.writeGraphCell(avgTBuffer, tMin, tMax, F("waterBar"), false);
        Html.writeRowEnd();
    }

    Html.writeTableEnd();
    Html.writeFooter();
}


void handleHttpCalibrateFormRequest()
{
    Tracer tracer(F("handleHttpCalibrateFormRequest"));
    ChunkedResponse response(HttpResponse, WebServer, ContentTypeHtml);

    Html.writeHeader(F("Calibrate sensors"), Nav);

    if (TempSensors.getDS18Count() < 2)
    {
        Html.writeHeading(F("Missing temperature sensors"), 2);
        HttpResponse.printf(F("<p>Number of temperature sensors detected: %d</p>\r\n"), TempSensors.getDS18Count());
    }
    else
    {
        Html.writeFormStart(F("/calibrate"));

        Html.writeTableStart();
        Html.writeRowStart();
        Html.writeHeaderCell(F("Sensor"));
        Html.writeHeaderCell(F("Measured"));
        Html.writeHeaderCell(F("Offset"));
        Html.writeHeaderCell(F("Effective"));
        Html.writeRowEnd();

        for (int i = 0; i < 3; i++)
        {
            MonitoredTopic topic = MonitoredTopics[i];
            float tMeasured = TempSensors.getTempC(PersistentData.tempSensorAddress[i]);
            if (tMeasured == DEVICE_DISCONNECTED_C) continue;

            Html.writeRowStart();
            Html.writeCell(topic.htmlLabel);
            Html.writeCell(tMeasured, F("%0.2f °C"));
            HttpResponse.printf(
                F("<td><input type=\"text\" name=\"%s\" value=\"%0.2f\" maxlength=\"5\"></td>"),
                topic.label,
                PersistentData.tempSensorOffset[i]);
            Html.writeCell(tMeasured + PersistentData.tempSensorOffset[i], F("%0.2f °C"));
            Html.writeRowEnd();
        }

        Html.writeTableEnd();    

        Html.writeTableStart();
        Html.writeCheckbox(F("swapInOut"), F("Swap input and output sensors"), false);
        if (PersistentData.isBufferEnabled())
            Html.writeCheckbox(F("swapInBuf"), F("Swap input and buffer sensors"), false);
        Html.writeTableEnd();

        Html.writeSubmitButton(F("Calibrate"));
        Html.writeFormEnd();
    }

    Html.writeFooter();
}


void handleHttpCalibrateFormPost()
{
    Tracer tracer(F("handleHttpCalibrateFormPost"));

    for (int i = 0; i < 3; i++)
    {
        String argName = MonitoredTopics[i].label;
        if (WebServer.hasArg(argName))
        {
            PersistentData.tempSensorOffset[i] = WebServer.arg(argName).toFloat();
        }
    }

    if (WebServer.hasArg("swapInOut"))
    {
        DeviceAddress tInputSensorAddress;
        memcpy(tInputSensorAddress, PersistentData.tempSensorAddress[TopicId::TInput], sizeof(DeviceAddress));
        memcpy(PersistentData.tempSensorAddress[TopicId::TInput], PersistentData.tempSensorAddress[TopicId::TOutput], sizeof(DeviceAddress));
        memcpy(PersistentData.tempSensorAddress[TopicId::TOutput], tInputSensorAddress, sizeof(DeviceAddress));
    }
    if (WebServer.hasArg("swapInBuf"))
    {
        DeviceAddress tInputSensorAddress;
        memcpy(tInputSensorAddress, PersistentData.tempSensorAddress[TopicId::TInput], sizeof(DeviceAddress));
        memcpy(PersistentData.tempSensorAddress[TopicId::TInput], PersistentData.tempSensorAddress[TopicId::TBuffer], sizeof(DeviceAddress));
        memcpy(PersistentData.tempSensorAddress[TopicId::TBuffer], tInputSensorAddress, sizeof(DeviceAddress));
    }

    PersistentData.validate();
    PersistentData.writeToEEPROM();

    newSensorFound = false;

    handleHttpCalibrateFormRequest();
}


void handleHttpEventLogRequest()
{
    Tracer tracer(F("handleHttpEventLogRequest"));
    ChunkedResponse response(HttpResponse, WebServer, ContentTypeHtml);

    if (WiFiSM.shouldPerformAction(F("clear")))
    {
        EventLog.clear();
        WiFiSM.logEvent(F("Event log cleared."));
    }

    Html.writeHeader(F("Event log"), Nav);

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

    if (!TempSensors.isConnected(PersistentData.tempSensorAddress[TopicId::TBuffer]))
        Html.writeParagraph(F("NOTE: No buffer sensor is connected. Leave 'T<sub>buffer, max</sub>' zero to suppress buffer temperature in UI."));

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

    PersistentData.parseHtmlFormData([](const String& id) -> String { return WebServer.arg(id); });
    PersistentData.validate();
    PersistentData.writeToEEPROM();

    handleHttpConfigFormRequest();
}


void writeCurrentValues()
{
    Html.writeSectionStart(F("Current values"));
    Html.writeTableStart();

    for (int i = 0; i < NUMBER_OF_TOPICS; i++)
    {
        MonitoredTopic topic = MonitoredTopics[i];
        float topicValue = currentValues[topic.id];

        String barCssClass = String(topic.style) + "Bar";
        String graphCssClass = String(F("graph fill"));

        if (topic.id == TopicId::TBuffer)
        {
            if (!PersistentData.isBufferEnabled()) continue;

            topic.maxValue = PersistentData.tBufferMax;
            graphCssClass += F(" wide");
        }

        Html.writeRowStart();
        Html.writeHeaderCell(topic.htmlLabel);
        Html.writeCell(topic.formatValue(topicValue, true));
        Html.writeCellStart(graphCssClass);
        Html.writeMeterDiv(topicValue, topic.minValue, topic.maxValue, barCssClass);
        Html.writeCellEnd();
        Html.writeRowEnd();
    }

    Html.writeTableEnd();
    Html.writeSectionEnd();
}


void writeDayStats()
{
    Html.writeSectionStart(F("Statistics per day"));

    Html.writeTableStart();
    DayStatsEntry::writeHeader(Html, PersistentData.isBufferEnabled());

    float maxEnergy = 0.1F; // Prevent division by zero
    for (DayStatsEntry& dayStatsEntry : DayStats)
    {
        maxEnergy = std::max(maxEnergy, dayStatsEntry.energyIn);
        maxEnergy = std::max(maxEnergy, dayStatsEntry.energyOut);
    }
    TRACE(F("maxEnergy: %0.1f\n"), maxEnergy);

    for (DayStatsEntry& dayStatsEntry : DayStats)
        dayStatsEntry.writeRow(Html, maxEnergy, PersistentData.isBufferEnabled());

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

    if (newSensorFound)
    {
        handleHttpCalibrateFormRequest();
        return;
    }

    if (WiFiSM.shouldPerformAction(F("valve")))
    {
        // Toggle valve state (through Web UI)
        setMaxTempValve(!maxTempValveActivated);
        maxTempValveOverride = maxTempValveActivated;
    }

    String ftpSync;
    if (PersistentData.ftpServer[0] == 0)
        ftpSync = F("Disabled");
    else if (lastFTPSyncTime == 0)
        ftpSync = F("Not yet");
    else
        ftpSync = formatTime("%H:%M", lastFTPSyncTime);

    ChunkedResponse response(HttpResponse, WebServer, ContentTypeHtml);
    Html.writeHeader(F("Home"), Nav);

    Html.writeDivStart(F("flex-container"));

    Html.writeSectionStart(F("Status"));
    Html.writeTableStart();
    Html.writeRow(F("WiFi RSSI"), F("%d dBm"), static_cast<int>(WiFi.RSSI()));
    Html.writeRow(F("Free Heap"), F("%0.1f kB"), float(ESP.getFreeHeap()) / 1024);
    Html.writeRow(F("Uptime"), F("%0.1f days"), float(WiFiSM.getUptime()) / SECONDS_PER_DAY);
    if (PersistentData.isBufferEnabled())
    {
        Html.writeRow(F("T<sub>buffer,max</sub>"), F("%0.1f °C"), PersistentData.tBufferMax);
        Html.writeRowStart();
        Html.writeHeaderCell(F("T<sub>max</sub> valve"));
        Html.writeCellStart("");
        Html.writeActionLink(F("valve"), maxTempValveActivated ? F("On") : F("Off"), currentTime);
        Html.writeCellEnd();
        Html.writeRowEnd();
    }
    Html.writeTableEnd();
    Html.writeSectionEnd();

    writeCurrentValues();
    writeDayStats();

    Html.writeDivEnd();
    Html.writeFooter();
}


void onTimeServerSynced()
{
    heatLogTime = currentTime;
    newHeatLogEntry();
    newDayStatsEntry();
}


void onWiFiInitialized()
{
    if (currentTime >= heatLogTime)
    {
        heatLogTime++;
        calculateValues();
        updateHeatLog();
        updateDayStats();
    }

    if ((syncFTPTime != 0) && (currentTime >= syncFTPTime) && WiFiSM.isConnected())
    {
        if (trySyncFTP(nullptr))
        {
            WiFiSM.logEvent(F("FTP sync"));
            syncFTPTime = 0;
        }
        else
        {
            WiFiSM.logEvent(F("FTP sync failed: %s"), FTPClient.getLastError());
            syncFTPTime += FTP_RETRY_INTERVAL;
        }
    }

}


// Boot code
void setup() 
{
    Serial.begin(74880); // Use same baudrate as bootloader
    Serial.setTimeout(1000);
    Serial.println("Boot"); // Flush garbage caused by ESP boot output.

    #ifdef DEBUG_ESP_PORT
    Tracer::traceTo(DEBUG_ESP_PORT);
    Tracer::traceFreeHeap();
    #endif

    BuiltinLED.begin(); // Turn built-in LED on

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
            .icon = Files[LogFileIcon],
            .label = PSTR("Event log"),
            .urlPath =PSTR("events"),
            .handler = handleHttpEventLogRequest
        },
        MenuItem
        {
            .icon = Files[GraphIcon],
            .label = PSTR("Heat log"),
            .urlPath = PSTR("heatlog"),
            .handler = handleHttpHeatLogRequest
        },
        MenuItem
        {
            .icon = Files[GraphIcon],
            .label = PSTR("Temperature log"),
            .urlPath = PSTR("templog"),
            .handler = handleHttpTempLogRequest
        },
        MenuItem
        {
            .icon = Files[GraphIcon],
            .label = PSTR("Buffer log"),
            .urlPath = PSTR("bufferlog"),
            .handler = handleHttpBufferLogRequest
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
            .icon = Files[CalibrateIcon],
            .label = PSTR("Calibrate"),
            .urlPath = PSTR("calibrate"),
            .handler= handleHttpCalibrateFormRequest,
            .postHandler = handleHttpCalibrateFormPost
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

    WebServer.on("/json", handleHttpJsonRequest);

    WiFiSM.registerStaticFiles(Files, _LastFile);    
    WiFiSM.on(WiFiInitState::TimeServerSynced, onTimeServerSynced);
    WiFiSM.on(WiFiInitState::Initialized, onWiFiInitialized);
    WiFiSM.scanAccessPoints();
    WiFiSM.begin(PersistentData.wifiSSID, PersistentData.wifiKey, PersistentData.hostName);

    Flow_Sensor.begin(5.0, 6.6); // 5 sec measure interval, 6.6 Hz @ 1 l/min
    Energy_Meter.begin(100, 1000, 10); // 100 W resolution, 1000 pulses per kWh, max 10 aggregations (=> 6 minutes max)
    TempSensors.begin();
    TempSensors.setWaitForConversion(false);

    initTempSensors();

    logSensorInfo(TopicId::TInput);
    logSensorInfo(TopicId::TOutput);
    if (PersistentData.isBufferEnabled())
    {
        logSensorInfo(TopicId::TBuffer);
        pinMode(MAX_TEMP_VALVE_PIN, OUTPUT);
        setMaxTempValve(false);
    }

    if (TempSensors.getDS18Count() != 0)
    {
        TempSensors.requestTemperatures();
        TempTicker.attach(1.0F, readTempSensors);
    }

    memset(testOverrides, 0, sizeof(testOverrides));

    Tracer::traceFreeHeap();

    BuiltinLED.setOn(false);
}


// Called repeatedly
void loop() 
{
    currentTime = WiFiSM.getCurrentTime();

    // Let WiFi State Machine handle initialization and web requests
    // This also calls the onXXX methods below
    WiFiSM.run();

    if (Serial.available())
    {
        String message = Serial.readStringUntil('\n');
        test(message);
    }
}
