#include <math.h>
#include <ESPWiFi.h>
#include <ESPWebServer.h>
#include <ESPFileSystem.h>
#include <WiFiStateMachine.h>
#include <WiFiNTP.h>
#include <WiFiFTP.h>
#include <TimeUtils.h>
#include <Tracer.h>
#include <StringBuilder.h>
#include <Navigation.h>
#include <HtmlWriter.h>
#include <LED.h>
#include <Log.h>
#include <Wire.h>
#include "PersistentData.h"
#include "OpenThermLogEntry.h"
#include "StatusLogEntry.h"
#include "HeatMonClient.h"
#include "WeatherAPI.h"
#include "OTGW.h"

#ifdef ESP8266
#include <AsyncHTTPRequest_Generic.h>
#endif

constexpr int SET_BOILER_RETRY_INTERVAL = 6;
constexpr int HTTP_POLL_INTERVAL = 60;
constexpr int EVENT_LOG_LENGTH = 50;
constexpr int OT_LOG_LENGTH = 250;
constexpr int OT_LOG_PAGE_SIZE = 50;
constexpr int PWM_PERIOD = 10 * SECONDS_PER_MINUTE;
constexpr int TSET_OVERRIDE_DURATION = 20 * SECONDS_PER_MINUTE;
constexpr int WEATHER_SERVICE_POLL_INTERVAL = 15 * SECONDS_PER_MINUTE;
constexpr int FTP_RETRY_INTERVAL = 15 * SECONDS_PER_MINUTE;
constexpr int HEATMON_POLL_INTERVAL = 1 * SECONDS_PER_MINUTE;
constexpr float MAX_HEATPUMP_POWER = 4.0; // kW
constexpr float MAX_PRESSURE = 3.0; // bar
constexpr float MAX_FLOW_RATE = 12.0; // l/min

#ifdef DEBUG_ESP_PORT
    constexpr int OTGW_TIMEOUT = 5 * SECONDS_PER_MINUTE;
    constexpr int OTGW_SETPOINT_OVERRIDE_TIMEOUT = 2 * SECONDS_PER_MINUTE;
    constexpr int OTGW_RESPONSE_TIMEOUT_MS = 5000;
#else
    constexpr int OTGW_TIMEOUT = 1 * SECONDS_PER_MINUTE;
    constexpr int OTGW_SETPOINT_OVERRIDE_TIMEOUT = 50; // seconds
    constexpr int OTGW_RESPONSE_TIMEOUT_MS = 2000;
#endif

#ifdef ESP8266
    constexpr uint8_t OTGW_RESET_PIN = 14; // on NodeMCU V1.0
    SimpleLED BuiltinLED(LED_BUILTIN, true);
#else
    constexpr uint8_t OTGW_RESET_PIN = SCK; // on D1-mini form factor
    RGBLED BuiltinLED(LED_BUILTIN);
#endif

#if defined(ESP8266) || defined(DEBUG_ESP_PORT)
    #define OTGW_SERIAL Serial
    #define OTGW_SERIAL_INIT Serial.begin(9600);
#else
    #define OTGW_SERIAL Serial1
    #define OTGW_SERIAL_INIT Serial1.begin(9600, SERIAL_8N1, RX, TX); Serial1.setDebugOutput(false);
#endif


enum FileId
{
    Logo,
    Styles,
    Home,
    Calibrate,
    Graph,
    LogFile,
    Settings,
    Upload,
    _Last
};

const char* Files[] PROGMEM =
{
    "Logo.png",
    "styles.css",
    "Home.svg",
    "Calibrate.svg",
    "Graph.svg",
    "LogFile.svg",
    "Settings.svg",
    "Upload.svg"
};


const char* ContentTypeHtml = "text/html;charset=UTF-8";
const char* ContentTypeJson = "application/json";
const char* ContentTypeText = "text/plain";
const char* ButtonClass = "button";

const char* BoilerLevelNames[] = {"Off", "Pump-only", "Low", "High", "Thermostat"};

enum BoilerLevel // Unscoped enum so it can be used as array index without casting
{
    Off = 0,
    PumpOnly = 1,
    Low = 2,
    High = 3,
    Thermostat = 4
};

int boilerTSet[] = {0, 15, 40, 60, 0}; // See initBoilerLevels()

const char* LogHeaders[] PROGMEM =
{
    "Time",
    "Status (t)",
    "Status (b)",
    "Max mod",
    "Tset (t)",
    "Tset (b)",
    "Tboiler",
    "Treturn",
    "Tbuffer",
    "Toutside",
    "Pheatpump",
    "Pressure",
    "Mod (%)",
    "Flow"
};

OpenThermGateway OTGW(OTGW_SERIAL, OTGW_RESET_PIN);
ESPWebServer WebServer(80); // Default HTTP port
WiFiNTP TimeServer;
WiFiFTPClient FTPClient(3000); // 3s timeout
HeatMonClient HeatMon;
WeatherAPI WeatherService;
StringBuilder HttpResponse(12 * 1024); // 12KB HTTP response buffer
HtmlWriter Html(HttpResponse, Files[FileId::Logo], Files[FileId::Styles], 40);
StringLog EventLog(EVENT_LOG_LENGTH, 128);
StaticLog<OpenThermLogEntry> OpenThermLog(OT_LOG_LENGTH);
StaticLog<StatusLogEntry> StatusLog(7); // 7 days
WiFiStateMachine WiFiSM(BuiltinLED, TimeServer, WebServer, EventLog);
Navigation Nav;

// OpenTherm data values indexed by data ID
uint16_t thermostatRequests[256];
uint16_t boilerResponses[256];
uint16_t otgwRequests[256];
uint16_t otgwResponses[256];

time_t currentTime = 0;
time_t updateLogTime = 0;
time_t otgwInitializeTime = OTGW_STARTUP_TIME;
time_t otgwTimeout = OTGW_TIMEOUT;
time_t heatmonPollTime = 0;
time_t lastHeatmonUpdateTime = 0;
time_t weatherServicePollTime = 0;
time_t lastWeatherUpdateTime = 0;
time_t flameSwitchedOnTime = 0;

time_t lowLoadLastOn = 0;
time_t lowLoadLastOff = 0;
uint32_t lowLoadPeriod = 0;
uint32_t lowLoadDutyInterval = 0;

bool pumpOff = false;
bool updateTOutside = false;
bool updateHeatmonData = false;
int lastHeatmonResult = 0;

OpenThermLogEntry newOTLogEntry;
OpenThermLogEntry* lastOTLogEntryPtr = nullptr;
StatusLogEntry* lastStatusLogEntryPtr = nullptr;

uint16_t otLogEntriesToSync = 0;
time_t syncFTPTime = 0;
time_t lastFTPSyncTime = 0;

BoilerLevel currentBoilerLevel = BoilerLevel::Thermostat;
BoilerLevel changeBoilerLevel;
time_t changeBoilerLevelTime = 0;
int setBoilerLevelRetries = 0; 
float pwmDutyCycle = 1;

String otgwResponse;

// Forward defines
void handleHttpConfigFormRequest();


void initBoilerLevels()
{
    boilerTSet[BoilerLevel::Low] = PersistentData.minTSet;
    if (boilerTSet[BoilerLevel::High] != PersistentData.maxTSet)
    {
        boilerTSet[BoilerLevel::High] = PersistentData.maxTSet;
        if (otgwInitializeTime == 0) otgwInitializeTime = currentTime;
    }    
}


void resetOtgwTraffic()
{
    memset(thermostatRequests, 0xFF, sizeof(thermostatRequests));
    memset(boilerResponses, 0xFF, sizeof(boilerResponses));
    memset(otgwRequests, 0xFF, sizeof(otgwRequests));
    memset(otgwResponses, 0xFF, sizeof(otgwResponses));
}


void resetOpenThermGateway()
{
    Tracer tracer(F("resetOpenThermGateway"));

    OTGW.reset();

    otgwInitializeTime = currentTime + OTGW_STARTUP_TIME;
    otgwTimeout = otgwInitializeTime + OTGW_TIMEOUT;

    currentBoilerLevel = BoilerLevel::Thermostat;

    WiFiSM.logEvent(F("OTGW reset"));
}


bool setOtgwResponse(OpenThermDataId dataId, float value)
{
    Tracer tracer(F("setOtgwResponse"));
    TRACE(F("dataId: %d, value:%0.1f\n"), dataId, value);

    bool success = OTGW.setResponse(dataId, value); 
    if (!success)
        WiFiSM.logEvent(F("OTGW error: %s"), OTGW.getLastError());

    return success;
}


void initializeOpenThermGateway()
{
    Tracer tracer(F("initializeOpenThermGateway"));

    bool success = setOtgwResponse(OpenThermDataId::MaxTSet, boilerTSet[BoilerLevel::High]);

    if (success)
        WiFiSM.logEvent(F("OTGW initialized"));
    else
        resetOpenThermGateway();
}


bool setBoilerLevel(BoilerLevel level, time_t duration)
{
    Tracer tracer(F("setBoilerLevel"), BoilerLevelNames[level]);
    TRACE(F("Duration: %d\n"), static_cast<int>(duration));

    if (duration != 0)
    {
        changeBoilerLevel = BoilerLevel::Thermostat;
        changeBoilerLevelTime = currentTime + duration;
    }

    if (level == currentBoilerLevel)
        return false;

    if ((changeBoilerLevelTime != 0) && (level == changeBoilerLevel))
        changeBoilerLevelTime = 0;

    bool success;
    if (level == BoilerLevel::Off)
        success = OTGW.sendCommand("CH", "0");
    else
    {
        success = OTGW.sendCommand("CS", String(boilerTSet[level]));

        if (success && (currentBoilerLevel == BoilerLevel::Off))
            success = OTGW.sendCommand("CH", "1");
    }

    if (success)
    {
        currentBoilerLevel = level;
        setBoilerLevelRetries = 0;
    }
    else if (++setBoilerLevelRetries < 3)
    {
        changeBoilerLevel = level;
        changeBoilerLevelTime = WiFiSM.getCurrentTime() + SET_BOILER_RETRY_INTERVAL;
        WiFiSM.logEvent(
            F("Unable to set boiler to %s. Retry %d at %s."),
            BoilerLevelNames[level],
            setBoilerLevelRetries,
            formatTime("%H:%M:%S", changeBoilerLevelTime));
    }
    else
    {
        WiFiSM.logEvent(
            F("Unable to set boiler to %s after %d attempts."),
            BoilerLevelNames[level],
            setBoilerLevelRetries);
        setBoilerLevelRetries = 0;
        resetOpenThermGateway();
    }

    return success;
}


bool setBoilerLevel(BoilerLevel level)
{
    return setBoilerLevel(level, 0);
}


void cancelOverride()
{
    pumpOff = false;
    pwmDutyCycle = 1;
    lowLoadLastOn = 0;
    lowLoadLastOff = 0;
    lowLoadPeriod = 0;
    lowLoadDutyInterval = 0;
    WiFiSM.logEvent(F("Override %s stopped"), BoilerLevelNames[currentBoilerLevel]);
    setBoilerLevel(BoilerLevel::Thermostat);
}


uint16_t getResponse(OpenThermDataId dataId)
{
    uint16_t result = otgwResponses[dataId];
    if (result == DATA_VALUE_NONE)
        result = boilerResponses[dataId];
    return result;
}


void updateStatusLog(time_t time, uint16_t status)
{
    if ((lastStatusLogEntryPtr == nullptr) ||
        (time / SECONDS_PER_DAY) > (lastStatusLogEntryPtr->startTime / SECONDS_PER_DAY))
    {
        time_t startOfDay = time - (time % SECONDS_PER_DAY);
        StatusLogEntry* logEntryPtr = new StatusLogEntry();
        logEntryPtr->startTime = startOfDay;
        logEntryPtr->stopTime = startOfDay;
        lastStatusLogEntryPtr = StatusLog.add(logEntryPtr);
        delete logEntryPtr;
    }

    if (status & OpenThermStatus::SlaveCHMode)
    {
        if (lastStatusLogEntryPtr->chSeconds++ == 0)
            lastStatusLogEntryPtr->startTime = time;
        lastStatusLogEntryPtr->stopTime = time;
    }
    if (status & OpenThermStatus::SlaveDHWMode)
        lastStatusLogEntryPtr->dhwSeconds++;
    if (status & OpenThermStatus::SlaveFlame)
        lastStatusLogEntryPtr->flameSeconds++;

    if (currentBoilerLevel != BoilerLevel::Thermostat)
        lastStatusLogEntryPtr->overrideSeconds++;
}


void logOpenThermValues(bool forceCreate)
{
    newOTLogEntry.time = currentTime;
    newOTLogEntry.thermostatTSet = thermostatRequests[OpenThermDataId::TSet];
    newOTLogEntry.thermostatMaxRelModulation = thermostatRequests[OpenThermDataId::MaxRelModulation];
    newOTLogEntry.boilerStatus = boilerResponses[OpenThermDataId::Status];
    newOTLogEntry.boilerTSet = boilerResponses[OpenThermDataId::TSet];
    newOTLogEntry.boilerRelModulation = boilerResponses[OpenThermDataId::RelModulation];
    newOTLogEntry.tBoiler = boilerResponses[OpenThermDataId::TBoiler];
    newOTLogEntry.tReturn = getResponse(OpenThermDataId::TReturn);
    if (newOTLogEntry.tReturn == DATA_VALUE_NONE || newOTLogEntry.tReturn == 0)
        newOTLogEntry.tReturn = HeatMon.tOut * 256;
    newOTLogEntry.tBuffer = getResponse(OpenThermDataId::Tdhw);
    newOTLogEntry.tOutside = getResponse(OpenThermDataId::TOutside);
    newOTLogEntry.pressure = boilerResponses[OpenThermDataId::Pressure];
    newOTLogEntry.flowRate = boilerResponses[OpenThermDataId::DHWFlowRate];
    if (newOTLogEntry.flowRate == 0)
        newOTLogEntry.flowRate = HeatMon.flowRate * 256;
    newOTLogEntry.pHeatPump = HeatMon.pIn * 256;

    if ((lastOTLogEntryPtr == nullptr) || !newOTLogEntry.equals(lastOTLogEntryPtr) || forceCreate)
    {
        lastOTLogEntryPtr = OpenThermLog.add(&newOTLogEntry);
        otLogEntriesToSync = std::min(otLogEntriesToSync + 1, OT_LOG_LENGTH);
        if (PersistentData.isFTPEnabled() && otLogEntriesToSync == PersistentData.ftpSyncEntries)
            syncFTPTime = currentTime;
    }
}


void writeCsvDataLine(OpenThermLogEntry* otLogEntryPtr, time_t time, Print& destination)
{
    int masterStatus = otLogEntryPtr->boilerStatus >> 8;
    int slaveStatus = otLogEntryPtr->boilerStatus & 0xFF;

    destination.print(formatTime("%F %H:%M:%S", time));
    destination.printf(";%d;%d", masterStatus, slaveStatus);
    destination.printf(";%d", OpenThermGateway::getInteger(otLogEntryPtr->thermostatMaxRelModulation));
    destination.printf(";%d", OpenThermGateway::getInteger(otLogEntryPtr->thermostatTSet));
    destination.printf(";%d", OpenThermGateway::getInteger(otLogEntryPtr->boilerTSet));
    destination.printf(";%0.1f", OpenThermGateway::getDecimal(otLogEntryPtr->tBoiler));
    destination.printf(";%0.1f", OpenThermGateway::getDecimal(otLogEntryPtr->tReturn));
    destination.printf(";%0.1f", OpenThermGateway::getDecimal(otLogEntryPtr->tBuffer));
    destination.printf(";%0.1f", OpenThermGateway::getDecimal(otLogEntryPtr->tOutside));
    destination.printf(";%0.2f", OpenThermGateway::getDecimal(otLogEntryPtr->pHeatPump));
    destination.printf(";%0.2f", OpenThermGateway::getDecimal(otLogEntryPtr->pressure));
    destination.printf(";%d", OpenThermGateway::getInteger(otLogEntryPtr->boilerRelModulation));
    destination.printf(";%0.1f\r\n", OpenThermGateway::getDecimal(otLogEntryPtr->flowRate));
}


void writeCsvDataLines(OpenThermLogEntry* otLogEntryPtr, OpenThermLogEntry* prevLogEntryPtr, Print& destination)
{
    while (otLogEntryPtr != nullptr)
    {
        time_t otLogEntryTime = otLogEntryPtr->time;
        time_t oneSecEarlier = otLogEntryTime - 1;
        if ((prevLogEntryPtr != nullptr) && (prevLogEntryPtr->time < oneSecEarlier))
        {
            // Repeat previous log entry, but one second before this one.
            // This enforces steep step transitions.
            writeCsvDataLine(prevLogEntryPtr, oneSecEarlier, destination);
        }
        writeCsvDataLine(otLogEntryPtr, otLogEntryTime, destination);
        
        prevLogEntryPtr = otLogEntryPtr;
        otLogEntryPtr = OpenThermLog.getNextEntry();
    }
}


bool trySyncFTP(Print* printTo)
{
    Tracer tracer(F("trySyncFTP"));

    FTPClient.beginAsync(
        PersistentData.ftpServer,
        PersistentData.ftpUser,
        PersistentData.ftpPassword,
        FTP_DEFAULT_CONTROL_PORT,
        printTo);

    auto otLogWriter = [printTo](Print& output)
    {
        if (otLogEntriesToSync > 0)
        {
            OpenThermLogEntry* prevLogEntryPtr = OpenThermLog.getEntryFromEnd(otLogEntriesToSync + 1);
            OpenThermLogEntry* otLogEntryPtr = OpenThermLog.getEntryFromEnd(otLogEntriesToSync);
            writeCsvDataLines(otLogEntryPtr, prevLogEntryPtr, output);
            otLogEntriesToSync = 0;
        }
        else if (printTo != nullptr)
            printTo->println(F("Nothing to sync."));
    };

    String filename = PersistentData.hostName;
    filename += ".csv";
    FTPClient.appendAsync(filename, otLogWriter);

   if (printTo == nullptr) return true; // Run async

    // Run synchronously
    bool success = FTPClient.run();
    if (success) lastFTPSyncTime = currentTime;
    return success;
}


void test(String message)
{
    Tracer tracer(F("test"));

    if (message.startsWith("testL"))
    {
        OTGW.feedWatchdog();
        for (int i = 0; i < EVENT_LOG_LENGTH; i++)
        {
            WiFiSM.logEvent(F("Test event"));
            yield();
        }
        Tracer::traceFreeHeap();
    }
    else if (message.startsWith("testO"))
    {

        OTGW.feedWatchdog();
        for (int i = 0; i < 50; i++)
        {
            logOpenThermValues(true);
            yield();
        }

        OTGW.feedWatchdog();
        for (int i = 0; i < OTGW_MESSAGE_LOG_LENGTH; i++)
        {
            char testMessage[12];
            snprintf(testMessage, sizeof(testMessage), "T%08X", i);
            OTGW.MessageLog.add(testMessage);
        }
    }
    else if (message.startsWith("testW"))
    {
        weatherServicePollTime = currentTime;
    }
    else if (message.startsWith("testS"))
    {
        time_t testTime = currentTime;
        for (int i = 0; i < 7; i++)
        {
            for (int j = 0; j < 90; j++)
            {
                uint16_t testStatus = OpenThermStatus::SlaveCHMode;
                if (j % 2 == 0) testStatus |= OpenThermStatus::SlaveFlame;
                updateStatusLog(testTime, testStatus);
            }
            testTime += SECONDS_PER_DAY;
        }
    }
}


bool handleThermostatLowLoadMode(bool switchedOn)
{
    bool isThermostatLowLoadMode = thermostatRequests[OpenThermDataId::MaxRelModulation] == 0;
    if (!isThermostatLowLoadMode) return false;

    if (switchedOn)
    {
        if (lowLoadLastOn != 0)
        {
            lowLoadPeriod = currentTime - lowLoadLastOn;
            lowLoadDutyInterval = lowLoadLastOff - lowLoadLastOn;
        }
        lowLoadLastOn = currentTime;
    }
    else
        lowLoadLastOff = currentTime;

    if (currentBoilerLevel == BoilerLevel::PumpOnly)
    {
        // Keep Pump Only level, but switch to Low level afterwards.
        if (changeBoilerLevel == BoilerLevel::Thermostat)
        {
            changeBoilerLevel = BoilerLevel::Low;
            WiFiSM.logEvent(F("Start low-load at %s"), formatTime("%H:%M", changeBoilerLevelTime));
        }
    }
    else if (!pumpOff)
    {
        if (currentBoilerLevel == BoilerLevel::Thermostat || pwmDutyCycle < 1)
        {
            pwmDutyCycle = 1;
            WiFiSM.logEvent(F("Low-load started"));
        }

        BoilerLevel newBoilerLevel = BoilerLevel::Low;;
        if (!switchedOn && PersistentData.usePumpModulation && !HeatMon.isHeatpumpOn() && currentBoilerLevel != BoilerLevel::Thermostat)
        {
            // Thermostat switched CH off, pump modulation is on and heatpump is off.
            // Switch CH/pump off, but keep the TSet override (for a while). 
            newBoilerLevel = BoilerLevel::Off;
        }
        setBoilerLevel(newBoilerLevel, TSET_OVERRIDE_DURATION);
    }

    return true;
}


bool checkInvalidStatus(const OpenThermGatewayMessage& otFrame)
{
    bool result = (otFrame.dataId == OpenThermDataId::Status) && (otFrame.dataValue & 0xFCF0); 
    if (result)
    {
        const char* source = otFrame.direction == OpenThermGatewayDirection::FromThermostat
            ? "thermostat"
            : "boiler";
        WiFiSM.logEvent(F("Invalid status from %s: 0x%04X"), source, otFrame.dataValue);
    }
    return result;
}


void handleThermostatRequest(const OpenThermGatewayMessage& otFrame)
{
    Tracer tracer(F("handleThermostatRequest"));
    
    if (checkInvalidStatus(otFrame)) return;

    if (otFrame.dataId == OpenThermDataId::Status)
    {
        bool masterCHEnable = otFrame.dataValue & OpenThermStatus::MasterCHEnable;
        bool lastMasterCHEnable = thermostatRequests[OpenThermDataId::Status] & OpenThermStatus::MasterCHEnable;
        if (masterCHEnable && !lastMasterCHEnable)
        {
            // Thermostat switched CH on
            if (!handleThermostatLowLoadMode(true))
            {
                // Thermostat is not in low load mode
                if (currentBoilerLevel != BoilerLevel::Thermostat)
                    cancelOverride();
                else if (PersistentData.boilerOnDelay != 0)
                {
                    // Keep boiler at Pump-only level for a while (give heatpump a headstart)
                    if (setBoilerLevel(BoilerLevel::PumpOnly, PersistentData.boilerOnDelay))
                        WiFiSM.logEvent(F("Pump-only until %s"), formatTime("%H:%M", changeBoilerLevelTime));
                }
            }
        }
        else if (!masterCHEnable && lastMasterCHEnable)
        {
            // Thermostat switched CH off
            if (!handleThermostatLowLoadMode(false))
            {
                // Thermostat is not in low load mode
                if (currentBoilerLevel != BoilerLevel::Thermostat)
                    cancelOverride();
            }
        }        
    }
    else if (otFrame.dataId == OpenThermDataId::MaxRelModulation)
    {
        if (otFrame.dataValue != 0 && thermostatRequests[OpenThermDataId::MaxRelModulation] == 0)
        {
            // Thermostat started requesting more than minimal Modulation (no longer in low-load mode).
            // If we're still in low-load mode and CH is on, we cancel the override here.
            // If the CH is off, it will be cancelled later when CH is switched on again.
            bool masterCHEnable = thermostatRequests[OpenThermDataId::Status] & OpenThermStatus::MasterCHEnable;
            if (masterCHEnable && currentBoilerLevel == BoilerLevel::Low && pwmDutyCycle == 1)
                cancelOverride();
        }
    }
    else if (otFrame.dataId == OpenThermDataId::TSet)
    {
        bool masterCHEnable = thermostatRequests[OpenThermDataId::Status] & OpenThermStatus::MasterCHEnable;
        bool isThermostatLowLoadMode = thermostatRequests[OpenThermDataId::MaxRelModulation] == 0;
        if (masterCHEnable && PersistentData.usePumpModulation && currentBoilerLevel != BoilerLevel::PumpOnly)
        {
            bool pwmPending = pwmDutyCycle < 1;
            float tSet = OpenThermGateway::getDecimal(otFrame.dataValue);
            float tPwmCeiling = std::min(std::max(HeatMon.tBuffer, (float)PersistentData.minTSet), (float)PersistentData.maxTSet);
            const float tPwmFloor = 20;
            if (tSet >= tPwmFloor && tSet < tPwmCeiling && !isThermostatLowLoadMode)
            {
                // Requested TSet in PWM range; use PWM.
                // We're continuously updating the PWM duty cycle, but this is picked up only when the current one is done.
                pwmDutyCycle = (tSet - tPwmFloor) / (tPwmCeiling - tPwmFloor);
                pwmDutyCycle = std::min(std::max(pwmDutyCycle, 0.1F), 0.9F); // Keep between 10% and 90%
                if (!pwmPending)
                {
                    WiFiSM.logEvent(F("PWM started: %0.0f%%. TSet=%0.1f < %0.1f"), pwmDutyCycle * 100, tSet, tPwmCeiling);
                    setBoilerLevel(BoilerLevel::Low, pwmDutyCycle * PWM_PERIOD);
                }
            }
            else if (pwmPending)
            {
                // Requested TSet is below 15 or above configured minimum; cancel PWM.
                WiFiSM.logEvent(F("PWM stopped. TSet=%0.1f"), tSet);
                cancelOverride();
            }
        }
    }

    thermostatRequests[otFrame.dataId] = otFrame.dataValue;
}


void handleBoilerResponse(const OpenThermGatewayMessage& otFrame)
{
    Tracer tracer(F("handleBoilerResponse"));

    if (otFrame.msgType == OpenThermMsgType::UnknownDataId) return;

    if (checkInvalidStatus(otFrame)) return;

    if (otFrame.dataId == OpenThermDataId::Status)
    {
        if (otFrame.dataValue & OpenThermStatus::SlaveFlame)
        {
            // Boiler flame is on
            if (boilerResponses[OpenThermDataId::Status] & OpenThermStatus::SlaveFlame)
            {
                if ((PersistentData.flameTimeout != 0) && (currentTime - flameSwitchedOnTime >= PersistentData.flameTimeout))
                {
                    if (setBoilerLevel(BoilerLevel::PumpOnly, TSET_OVERRIDE_DURATION))
                        WiFiSM.logEvent(F("Flame timeout. Pump-only until %s"), formatTime("%H:%M", changeBoilerLevelTime));
                }
            }
            else
                // Boiler flame is switched on
                flameSwitchedOnTime = currentTime;
        }
        else
            // Boiler flame is off
            flameSwitchedOnTime = 0;
    }

    boilerResponses[otFrame.dataId] = otFrame.dataValue;
}


void handleBoilerRequest(const OpenThermGatewayMessage& otFrame)
{
    Tracer tracer(F("handleBoilerRequest"));

    // Modified request from OTGW to boiler (e.g. TSet override)
    otgwRequests[otFrame.dataId] = otFrame.dataValue;
}


void handleThermostatResponse(const OpenThermGatewayMessage& otFrame)
{
    Tracer tracer(F("handleThermostatResponse"));

    if (otFrame.msgType == OpenThermMsgType::UnknownDataId) return;

    // Modified response from OTGW to thermostat (e.g. TOutside override)
    otgwResponses[otFrame.dataId] = otFrame.dataValue;

    if (otFrame.dataId == OpenThermDataId::MaxTSet)
    {
        int8_t maxTSet = OpenThermGateway::getInteger(otFrame.dataValue);
        if (maxTSet != boilerTSet[BoilerLevel::High])
        {
            WiFiSM.logEvent(F("Max CH Water setpoint is changed (by OTGW reset?)"));
            otgwInitializeTime = currentTime;
        }
    }
}


void onMessageReceived(const OpenThermGatewayMessage& otgwMessage)
{
    Tracer tracer(F("onMessageReceived"), otgwMessage.message.c_str());

    otgwTimeout = currentTime + OTGW_TIMEOUT;

    switch (otgwMessage.direction)
    {
        case OpenThermGatewayDirection::FromThermostat:
            handleThermostatRequest(otgwMessage);
            break;

        case OpenThermGatewayDirection::FromBoiler:
            handleBoilerResponse(otgwMessage);
            break;

        case OpenThermGatewayDirection::ToBoiler:
            handleBoilerRequest(otgwMessage);
            break;

        case OpenThermGatewayDirection::ToThermostat:
            handleThermostatResponse(otgwMessage);
            break;

        case OpenThermGatewayDirection::Unexpected:
            if (otgwMessage.message.startsWith("test"))
            {
                test(otgwMessage.message);
                break;
            }
            [[fallthrough]];

        case OpenThermGatewayDirection::Error:
            WiFiSM.logEvent(F("OTGW: '%s'"), otgwMessage.message.c_str());
    }
}


float getBarValue(float t, float tMin = 20, float tMax = 0)
{
    if (tMax == 0) tMax = boilerTSet[BoilerLevel::High];
    return std::max(t - tMin, 0.0F) / (tMax - tMin);
}


void writeOpenThermTemperatureRow(String label, String cssClass, uint16_t dataValue, float tMin = 20, float tMax = 0)
{
    float value = OpenThermGateway::getDecimal(dataValue);
    float barValue = getBarValue(value, tMin, tMax);

    Html.writeRowStart();
    Html.writeHeaderCell(label);
    Html.writeCell(value, F("%0.1f °C"));
    Html.writeGraphCell(barValue, cssClass, true);
    Html.writeRowEnd();
}


void writeCurrentValues()
{
    Html.writeSectionStart(F("Current values"));
    Html.writeTableStart();

    if (lastOTLogEntryPtr != nullptr)
    {
        bool flame = boilerResponses[OpenThermDataId::Status] & OpenThermStatus::SlaveFlame;
        float maxRelMod = OpenThermGateway::getDecimal(lastOTLogEntryPtr->thermostatMaxRelModulation);
        float relMod = OpenThermGateway::getDecimal(lastOTLogEntryPtr->boilerRelModulation);        
        float thermostatTSet = OpenThermGateway::getDecimal(lastOTLogEntryPtr->thermostatTSet);
        float boilerTset = OpenThermGateway::getDecimal(lastOTLogEntryPtr->boilerTSet);

        Html.writeRowStart();
        Html.writeHeaderCell(F("Thermostat"));
        if (thermostatRequests[OpenThermDataId::Status] & OpenThermStatus::MasterCHEnable)
            Html.writeCell(F("%0.1f °C @ %0.0f %%"), thermostatTSet, maxRelMod);
        else
            Html.writeCell(F("CH off"));
        Html.writeGraphCell(getBarValue(thermostatTSet), F("setBar"), true);
        Html.writeRowEnd();

        Html.writeRowStart();
        Html.writeHeaderCell(F("Boiler"));
        if (boilerResponses[OpenThermDataId::Status] & 0xFF)
            Html.writeCell(F("%0.1f °C @ %0.0f %%"), boilerTset, relMod);
        else
            Html.writeCell(F("Off"));
        Html.writeGraphCell(getBarValue(boilerTset), F("setBar"), true);
        Html.writeRowEnd();

        writeOpenThermTemperatureRow(F("T<sub>boiler</sub>"), flame ? F("flameBar") : F("waterBar"), lastOTLogEntryPtr->tBoiler); 
        writeOpenThermTemperatureRow(F("T<sub>return</sub>"), F("waterBar"), lastOTLogEntryPtr->tReturn); 
        writeOpenThermTemperatureRow(F("T<sub>buffer</sub>"), F("waterBar"), lastOTLogEntryPtr->tBuffer); 
        writeOpenThermTemperatureRow(F("T<sub>outside</sub>"), F("outsideBar"), lastOTLogEntryPtr->tOutside, -10, 30);

        float pressure = OpenThermGateway::getDecimal(lastOTLogEntryPtr->pressure);
        Html.writeRowStart();
        Html.writeHeaderCell(F("Pressure"));
        Html.writeCell(pressure, F("%0.2f bar"));
        Html.writeGraphCell(pressure / MAX_PRESSURE, F("pressureBar"), true);
        Html.writeRowEnd();

        float flowRate = OpenThermGateway::getDecimal(lastOTLogEntryPtr->flowRate);
        Html.writeRowStart();
        Html.writeHeaderCell(F("Flow"));
        Html.writeCell(flowRate, F("%0.1f l/min"));
        Html.writeGraphCell(flowRate / MAX_FLOW_RATE, F("waterBar"), true);
        Html.writeRowEnd();
    }

    if (HeatMon.isInitialized)
    {
        Html.writeRowStart();
        Html.writeHeaderCell(F("P<sub>heatpump</sub>"));
        Html.writeCell(HeatMon.pIn, F("%0.2f kW"));
        Html.writeGraphCell(HeatMon.pIn / MAX_HEATPUMP_POWER, F("powerBar"), true);
        Html.writeRowEnd();
    }

    time_t overrideTimeLeft =(changeBoilerLevelTime == 0) ? 0 : changeBoilerLevelTime - currentTime;
    const char* duration = (overrideTimeLeft == 0) ? "" : formatTimeSpan(overrideTimeLeft, false); 
    Html.writeRowStart();
    Html.writeHeaderCell(F("Override"));
    Html.writeCell(F("%s %s"), BoilerLevelNames[currentBoilerLevel], duration);
    Html.writeGraphCell(float(overrideTimeLeft) / TSET_OVERRIDE_DURATION, F("overrideBar"), true);
    Html.writeRowEnd();

    if (lowLoadPeriod != 0 || pwmDutyCycle < 1)
    {
        float dutyCycle = (pwmDutyCycle < 1) ? pwmDutyCycle : float(lowLoadDutyInterval) / lowLoadPeriod;
        const char* pwmType = (pwmDutyCycle < 1) ? "Forced" : "Low-load";

        Html.writeRowStart();
        Html.writeHeaderCell(F("PWM"));
        Html.writeCell(F("%s %0.0f %%"), pwmType, dutyCycle * 100);
        Html.writeGraphCell(dutyCycle, F("overrideBar"), true);
        Html.writeRowEnd();
    }

    Html.writeTableEnd();
    Html.writeSectionEnd();
}


uint32_t getMaxFlameSeconds()
{
    uint32_t result = 0;
    StatusLogEntry* logEntryPtr = StatusLog.getFirstEntry();
    while (logEntryPtr != nullptr)
    {
        result = std::max(result, logEntryPtr->flameSeconds);
        logEntryPtr = StatusLog.getNextEntry();
    }
    return result;
}


void writeStatisticsPerDay()
{
    Html.writeSectionStart(F("Statistics per day"));

    Html.writeTableStart();
    Html.writeRowStart();
    Html.writeHeaderCell(F("Day"));
    Html.writeHeaderCell(F("CH start"));
    Html.writeHeaderCell(F("CH stop"));
    Html.writeHeaderCell(F("Override"));
    Html.writeHeaderCell(F("CH on"));
    Html.writeHeaderCell(F("DHW on"));
    Html.writeHeaderCell(F("Flame"));
    Html.writeRowEnd();

    uint32_t maxFlameSeconds = getMaxFlameSeconds() + 1; // Prevent division by zero
    StatusLogEntry* logEntryPtr = StatusLog.getFirstEntry();
    while (logEntryPtr != nullptr)
    {
        Html.writeRowStart();
        Html.writeCell(formatTime("%a", logEntryPtr->startTime));
        Html.writeCell(formatTime("%H:%M", logEntryPtr->startTime));
        Html.writeCell(formatTime("%H:%M", logEntryPtr->stopTime));
        Html.writeCell(formatTimeSpan(logEntryPtr->overrideSeconds));
        Html.writeCell(formatTimeSpan(logEntryPtr->chSeconds));
        Html.writeCell(formatTimeSpan(logEntryPtr->dhwSeconds));
        Html.writeCell(formatTimeSpan(logEntryPtr->flameSeconds));
        Html.writeCellStart(F("graph"));
        Html.writeBar(float(logEntryPtr->flameSeconds) / maxFlameSeconds, F("flameBar"), false, false);
        Html.writeCellEnd();
        Html.writeRowEnd();

        logEntryPtr = StatusLog.getNextEntry();
    }

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

    uint32_t otgwErrors = 0;
    for (int i = 0; i <= 4; i++)
        otgwErrors += OTGW.errors[i];

    String ftpSyncTime;
    if (!PersistentData.isFTPEnabled())
        ftpSyncTime = F("Disabled");
    else if (lastFTPSyncTime == 0)
        ftpSyncTime = F("Not yet");
    else
        ftpSyncTime = formatTime("%H:%M", lastFTPSyncTime);

    Html.writeHeader(F("Home"), Nav, HTTP_POLL_INTERVAL);

    Html.writeDivStart(F("flex-container"));

    Html.writeSectionStart(F("Status"));
    Html.writeTableStart();
    Html.writeRow(F("WiFi RSSI"), F("%d dBm"), static_cast<int>(WiFi.RSSI()));
    Html.writeRow(F("Free Heap"), F("%0.1f kB"), float(ESP.getFreeHeap()) / 1024);
    Html.writeRow(F("Uptime"), F("%0.1f days"), float(WiFiSM.getUptime()) / SECONDS_PER_DAY);
    Html.writeRow(F("OTGW Errors"), F("%u"), otgwErrors);
    Html.writeRow(F("OTGW Resets"), F("%u"), OTGW.resets);
    Html.writeRow(F("FTP Sync"), F("%s"), ftpSyncTime.c_str());
    Html.writeRow(F("Sync entries"), F("%d / %d"), otLogEntriesToSync, PersistentData.ftpSyncEntries);
    if (lastHeatmonUpdateTime != 0)
        Html.writeRow(F("HeatMon"), F("%s"), formatTime("%H:%M", lastHeatmonUpdateTime));
    if (lastWeatherUpdateTime != 0)
        Html.writeRow(F("Weather"), F("%s"), formatTime("%H:%M", lastWeatherUpdateTime));
    Html.writeTableEnd();
    Html.writeSectionEnd();

    writeCurrentValues();
    writeStatisticsPerDay();

    Html.writeDivEnd();
    Html.writeFooter();

    WebServer.send(200, ContentTypeHtml, HttpResponse.c_str());
}


void handleHttpOpenThermRequest()
{
    Tracer tracer(F("handleHttpOpenThermRequest"));

    uint16_t burnerStarts = boilerResponses[OpenThermDataId::BoilerBurnerStarts];
    float avgBurnerOnTime;
    if ((burnerStarts == DATA_VALUE_NONE) || (burnerStarts == 0))
        avgBurnerOnTime = 0.0;
    else
    {
        int totalBurnerHours = boilerResponses[OpenThermDataId::BoilerBurnerHours] 
            + boilerResponses[OpenThermDataId::BoilerDHWBurnerHours];
        avgBurnerOnTime =  float(totalBurnerHours * 3600) / burnerStarts; 
    }

    Html.writeHeader(F("OpenTherm data"), Nav);

    Html.writeDivStart(F("flex-container"));

    Html.writeSectionStart(F("Thermostat"));
    Html.writeTableStart();
    Html.writeRow(F("Status"), F("%s"), OTGW.getMasterStatus(thermostatRequests[OpenThermDataId::Status]));
    Html.writeRow(F("TSet"), F("%0.1f °C"), OpenThermGateway::getDecimal(thermostatRequests[OpenThermDataId::TSet]));
    Html.writeRow(F("Max Modulation"), F("%0.1f %%"), OpenThermGateway::getDecimal(thermostatRequests[OpenThermDataId::MaxRelModulation]));
    Html.writeRow(F("Max TSet"), F("%0.1f °C"), OpenThermGateway::getDecimal(thermostatRequests[OpenThermDataId::MaxTSet]));
    Html.writeTableEnd();
    Html.writeSectionEnd();

    Html.writeSectionStart(F("Boiler"));
    Html.writeTableStart();
    Html.writeRow(F("Status"), F("%s"), OTGW.getSlaveStatus(boilerResponses[OpenThermDataId::Status]));
    Html.writeRow(F("TSet"), F("%0.1f °C"), OpenThermGateway::getDecimal(boilerResponses[OpenThermDataId::TSet]));
    Html.writeRow(F("Fault flags"), F("%s"), OTGW.getFaultFlags(boilerResponses[OpenThermDataId::SlaveFault]));
    Html.writeRow(F("Burner starts"), F("%d"), burnerStarts);
    Html.writeRow(F("Burner on"), F("%d h"), boilerResponses[OpenThermDataId::BoilerBurnerHours]);
    Html.writeRow(F("Avg burner on"), F("%s"), formatTimeSpan(avgBurnerOnTime));
    if (flameSwitchedOnTime != 0)
        Html.writeRow(F("Flame on"), F("%s"), formatTime("%H:%M:%S", flameSwitchedOnTime));
    Html.writeTableEnd();
    Html.writeSectionEnd();

    Html.writeSectionStart(F("Boiler override"));
    Html.writeTableStart();
    Html.writeRow(F("Current level"), F("%s"), BoilerLevelNames[currentBoilerLevel]);
    if (changeBoilerLevelTime != 0)
    {
        Html.writeRow(F("Change to"), F("%s"), BoilerLevelNames[changeBoilerLevel]);
        Html.writeRow(F("Change at"), F("%s"), formatTime("%H:%M:%S", changeBoilerLevelTime));
    }
    if (flameSwitchedOnTime != 0 && PersistentData.flameTimeout != 0)
    {
        time_t flameTimeoutAt = flameSwitchedOnTime + PersistentData.flameTimeout;
        Html.writeRow(F("Flame timeout"), F("%s"), formatTime("%H:%M:%S", flameTimeoutAt));
    }
    Html.writeTableEnd();
    Html.writeSectionEnd();

    Html.writeSectionStart(F("Low-load mode"));
    Html.writeTableStart();
    if (lowLoadLastOn != 0)
        Html.writeRow(F("Last on"), F("%s"), formatTime("%H:%M:%S", lowLoadLastOn));
    if (lowLoadLastOff != 0)
        Html.writeRow(F("Last off"), F("%s"), formatTime("%H:%M:%S", lowLoadLastOff));
    if (lowLoadPeriod != 0)
        Html.writeRow(F("Period"), F("%s"), formatTimeSpan(lowLoadPeriod, false));
    if (lowLoadDutyInterval != 0)
        Html.writeRow(F("Duty"), F("%s"), formatTimeSpan(lowLoadDutyInterval, false));
    Html.writeTableEnd();
    Html.writeSectionEnd();

    Html.writeDivEnd();

    Html.writeLink(F("/traffic"), F("View all OpenTherm traffic"), ButtonClass);

    Html.writeFooter();

    WebServer.send(200, ContentTypeHtml, HttpResponse.c_str());
}


void writeHtmlOpenThermDataTable(const String& title, uint16_t* otDataTable)
{
    Html.writeSectionStart(title);
    Html.writeTableStart();

    Html.writeRowStart();
    Html.writeHeaderCell(F("ID"));
    Html.writeHeaderCell(F("Hex value"));
    Html.writeHeaderCell(F("Dec value"));
    Html.writeRowEnd();

    for (int i = 0; i < 256; i++)
    {
        uint16_t dataValue = otDataTable[i];
        if (dataValue == DATA_VALUE_NONE) continue;
        Html.writeRowStart();
        Html.writeCell(i);
        Html.writeCell(F("%04X"), dataValue);
        Html.writeCell(F("%0.2f"), OpenThermGateway::getDecimal(dataValue));
        Html.writeRowEnd();
    }

    Html.writeTableEnd();
    Html.writeSectionEnd();
}


void handleHttpPumpRequest()
{
   Tracer tracer(F("handleHttpPumpRequest"));

    if (WebServer.hasArg("off"))
    {
        if (currentBoilerLevel == BoilerLevel::Low || currentBoilerLevel == BoilerLevel::Off)
        {
            pumpOff = true;
            setBoilerLevel(BoilerLevel::Off, TSET_OVERRIDE_DURATION);
            String reason = WebServer.arg("reason");
            WiFiSM.logEvent(F("Pump off: %s"), reason.c_str());
        }
    }
    else if (pumpOff)
    {
        pumpOff = false;
        setBoilerLevel(BoilerLevel::Low, TSET_OVERRIDE_DURATION);
        WiFiSM.logEvent(F("Pump resume"));
    }

    HttpResponse.clear();
    HttpResponse.printf(F("\"%s\""), BoilerLevelNames[currentBoilerLevel]);

    WebServer.send(200, ContentTypeJson, HttpResponse.c_str());
}


void handleHttpOpenThermTrafficRequest()
{
    Tracer tracer(F("handleHttpOpenThermTrafficRequest"));

    Html.writeHeader(F("OpenTherm traffic"), Nav);
    
    Html.writeDivStart(F("flex-container"));

    writeHtmlOpenThermDataTable(F("Thermostat requests"), thermostatRequests);
    writeHtmlOpenThermDataTable(F("Thermostat overrides"), otgwRequests);
    writeHtmlOpenThermDataTable(F("Boiler responses"), boilerResponses);
    writeHtmlOpenThermDataTable(F("Boiler overrides"), otgwResponses);

    Html.writeDivEnd();
    Html.writeFooter();

    WebServer.send(200, ContentTypeHtml, HttpResponse.c_str());
}


void handleHttpOpenThermLogRequest()
{
    Tracer tracer(F("handleHttpOpenThermLogRequest"));

    Html.writeHeader(F("OpenTherm log"), Nav);
    
    int currentPage = WebServer.hasArg("page") ? WebServer.arg("page").toInt() : 0;
    int totalPages = ((OpenThermLog.count() - 1) / OT_LOG_PAGE_SIZE) + 1;
    Html.writePager(totalPages, currentPage);

    Html.writeTableStart();
    Html.writeRowStart();
    for (PGM_P header : LogHeaders)
    {
        Html.writeHeaderCell(FPSTR(header));
    }
    Html.writeRowEnd();

    OpenThermLogEntry* otLogEntryPtr = OpenThermLog.getFirstEntry();
    for (int i = 0; i < (currentPage * OT_LOG_PAGE_SIZE) && otLogEntryPtr != nullptr; i++)
    {
        otLogEntryPtr = OpenThermLog.getNextEntry();
    }
    for (int j = 0; j < OT_LOG_PAGE_SIZE && otLogEntryPtr != nullptr; j++)
    {
        Html.writeRowStart();
        Html.writeCell(formatTime("%H:%M:%S", otLogEntryPtr->time));
        Html.writeCell(OTGW.getMasterStatus(otLogEntryPtr->boilerStatus));
        Html.writeCell(OTGW.getSlaveStatus(otLogEntryPtr->boilerStatus));
        Html.writeCell(OpenThermGateway::getInteger(otLogEntryPtr->thermostatMaxRelModulation));
        Html.writeCell(OpenThermGateway::getInteger(otLogEntryPtr->thermostatTSet));
        Html.writeCell(OpenThermGateway::getInteger(otLogEntryPtr->boilerTSet));
        Html.writeCell(OpenThermGateway::getDecimal(otLogEntryPtr->tBoiler));
        Html.writeCell(OpenThermGateway::getDecimal(otLogEntryPtr->tReturn));
        Html.writeCell(OpenThermGateway::getDecimal(otLogEntryPtr->tBuffer));
        Html.writeCell(OpenThermGateway::getDecimal(otLogEntryPtr->tOutside));
        Html.writeCell(OpenThermGateway::getDecimal(otLogEntryPtr->pHeatPump), F("%0.2f"));
        Html.writeCell(OpenThermGateway::getDecimal(otLogEntryPtr->pressure), F("%0.2f"));
        Html.writeCell(OpenThermGateway::getInteger(otLogEntryPtr->boilerRelModulation));
        Html.writeCell(OpenThermGateway::getDecimal(otLogEntryPtr->flowRate));
        Html.writeRowEnd();

        otLogEntryPtr = OpenThermLog.getNextEntry();
    }

    Html.writeTableEnd();
    Html.writeFooter();

    WebServer.send(200, ContentTypeHtml, HttpResponse.c_str());
}


void handleHttpFTPSyncRequest()
{
    Tracer tracer(F("handleHttpFTPSyncRequest"));

    Html.writeHeader(F("FTP Sync"), Nav);

    Html.writeParagraph(
        F("Sending %d OpenTherm log entries to FTP server (%s) ..."),
        otLogEntriesToSync,
        PersistentData.ftpServer);

    Html.writePreStart();
    bool success = trySyncFTP(&HttpResponse);
    Html.writePreEnd(); 

    if (success)
    {
        Html.writeParagraph(F("Success! Duration: %u ms"), FTPClient.getDurationMs());
        syncFTPTime = 0; // Cancel scheduled sync (if any)
    }
    else
        Html.writeParagraph(F("Failed: %s"), FTPClient.getLastError());
 
    Html.writeHeading(F("CSV header"), 2);
    Html.writePreStart();
    for (PGM_P header : LogHeaders)
    {
        HttpResponse.print(FPSTR(header));
        HttpResponse.print(';');
    }
    Html.writePreEnd();

    Html.writeFooter();

    WebServer.send(200, ContentTypeHtml, HttpResponse.c_str());
}


void handleHttpOTGWMessageLogRequest()
{
    Tracer tracer(F("handleHttpOTGWMessageLogRequest"));

    HttpResponse.clear();

    const char* otgwMessage = OTGW.MessageLog.getFirstEntry();
    while (otgwMessage != nullptr)
    {
        HttpResponse.println(otgwMessage);
        otgwMessage = OTGW.MessageLog.getNextEntry();
    }

    OTGW.MessageLog.clear();

    WebServer.send(200, ContentTypeText, HttpResponse.c_str());
}


void handleHttpEventLogRequest()
{
    Tracer tracer(F("handleHttpEventLogRequest"));

    if (WiFiSM.shouldPerformAction(F("clear")))
    {
        EventLog.clear();
        WiFiSM.logEvent(F("Event log cleared."));
    }

    Html.writeHeader(F("Event log"), Nav);

    const char* event = EventLog.getFirstEntry();
    while (event != nullptr)
    {
        Html.writeDiv(F("%s"), event);
        event = EventLog.getNextEntry();
    }

    Html.writeActionLink(F("clear"), F("Clear event log"), currentTime, ButtonClass);

    Html.writeFooter();

    WebServer.send(200, ContentTypeHtml, HttpResponse.c_str());
}


void handleHttpCommandFormRequest()
{
    Tracer tracer(F("handleHttpCommandFormRequest"));

    String cmd = WebServer.arg(F("cmd"));
    String value = WebServer.arg(F("value"));

    if (cmd.length() == 0) 
    {
        cmd = F("PR");
        value = F("A");
    }

    String tsetLowHref = F("?cmd=CS&value=");
    tsetLowHref += boilerTSet[BoilerLevel::Low];

    Html.writeHeader(F("OTGW Command"), Nav);

    Html.writeLink(F("?cmd=PR&value=A"), F("OTGW version"), ButtonClass);
    Html.writeLink(tsetLowHref, F("Set boiler level Low"), ButtonClass);
    Html.writeLink(F("?cmd=CS&value=0"), F("Stop override"), ButtonClass);

    Html.writeFormStart(F("cmd"));
    Html.writeTableStart();
    Html.writeTextBox(F("cmd"), F("Command"), cmd, 2);
    Html.writeTextBox(F("value"), F("Value"), value, 16);
    Html.writeTableEnd();
    Html.writeSubmitButton(F("Send command"));
    Html.writeFormEnd();

    Html.writeHeading(F("OTGW Response"), 2);
    Html.writePreStart();
    HttpResponse.print(otgwResponse);
    Html.writePreEnd();

    Html.writeFooter();

    otgwResponse = String();

    WebServer.send(200, ContentTypeHtml, HttpResponse.c_str());
}


void handleHttpCommandFormPost()
{
    Tracer tracer(("handleHttpCommandFormPost"));

    String cmd = WebServer.arg("cmd");
    String value = WebServer.arg("value");

    TRACE(F("cmd: '%s' value: '%s'\n"), cmd.c_str(), value.c_str());

    if (cmd.length() != 2)
        otgwResponse = F("Invalid command. Must be 2 characters.");
    else
    {
        if (OTGW.sendCommand(cmd, value))
            otgwResponse = OTGW.getResponse();
        else
            otgwResponse = OTGW.getLastError();
    }

    handleHttpCommandFormRequest();
}


void handleHttpConfigFormRequest()
{
    Tracer tracer(F("handleHttpConfigFormRequest"));

    Html.writeHeader(F("Settings"), Nav);

    Html.writeFormStart(F("/config"));
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

    WebServer.send(200, ContentTypeHtml, HttpResponse.c_str());
}


void handleHttpConfigFormPost()
{
    Tracer tracer(F("handleHttpConfigFormPost"));

    PersistentData.parseHtmlFormData([](const String& id) -> const String { return WebServer.arg(id); });
    PersistentData.validate();
    PersistentData.writeToEEPROM();

    initBoilerLevels();

    handleHttpConfigFormRequest();
}

void onTimeServerInit()
{
    // Time server initialization (DNS lookup) make take a few seconds
    // Feed OTGW watchdog just before to prevent getting bitten
    OTGW.feedWatchdog();
}


void onTimeServerSynced()
{
    // After time server sync all times leap ahead from 1-1-1970
    otgwTimeout = currentTime + OTGW_TIMEOUT;
    if (otgwInitializeTime != 0) 
        otgwInitializeTime = currentTime + OTGW_STARTUP_TIME;
    if (changeBoilerLevelTime != 0) 
        changeBoilerLevelTime = currentTime;
    
    updateLogTime = currentTime + 1;
    heatmonPollTime = currentTime + 10;
    weatherServicePollTime = currentTime + 15;
    OTGW.useLED(BuiltinLED);
}


void onWiFiUpdating()
{
    // Feed the OTGW watchdog just before OTA update
    OTGW.feedWatchdog();
}


void onWiFiInitialized()
{
    if (BuiltinLED.isOn())
    {
        if (!HeatMon.isRequestPending() && !WeatherService.isRequestPending() && !FTPClient.isAsyncPending())
            BuiltinLED.setOff();
    }
    else
    {
        if (HeatMon.isRequestPending())
            BuiltinLED.setColor(LED_YELLOW);
        else if (WeatherService.isRequestPending())
            BuiltinLED.setColor(LED_CYAN);
        else if (FTPClient.isAsyncPending())
            BuiltinLED.setColor(LED_MAGENTA);
    }

    if (currentTime >= updateLogTime)
    {
        // Update Status & OT logs every second
        updateLogTime++;
        updateStatusLog(currentTime, boilerResponses[OpenThermDataId::Status]);
        logOpenThermValues(false);
    }

    // Stuff below needs a WiFi connection. If the connection is lost, we skip it.
    if (WiFiSM.getState() < WiFiInitState::Connected)
    {
        HeatMon.setOffline();
        return;
    }

    if ((currentTime >= heatmonPollTime) && HeatMon.isInitialized)
    {
        // Get data from HeatMon
        int result = HeatMon.requestData();
        if (result == HTTP_OK)
        {
            updateHeatmonData = true;
            lastHeatmonUpdateTime = currentTime;
            lastHeatmonResult = result;
            heatmonPollTime = currentTime + HEATMON_POLL_INTERVAL;
        }
        else if (result != HTTP_REQUEST_PENDING)
        {
            if (result != lastHeatmonResult)
                WiFiSM.logEvent(F("HeatMon: %s"), HeatMon.getLastError().c_str());
            lastHeatmonResult = result;
            heatmonPollTime = currentTime + HEATMON_POLL_INTERVAL;
        }
    }

    if ((currentTime >= weatherServicePollTime) && WeatherService.isInitialized)
    {
        // Get outside temperature from Weather Service
        int result = WeatherService.requestData();
        if (result == HTTP_OK)
        {
            float currentTOutside = OpenThermGateway::getDecimal(getResponse(OpenThermDataId::TOutside));
            updateTOutside = (WeatherService.temperature != currentTOutside);
            lastWeatherUpdateTime = currentTime;
            weatherServicePollTime = currentTime + WEATHER_SERVICE_POLL_INTERVAL;
        }
        else if (result != HTTP_REQUEST_PENDING)
        {
            WiFiSM.logEvent(F("Weather: %s"), WeatherService.getLastError().c_str());
            weatherServicePollTime = currentTime + WEATHER_SERVICE_POLL_INTERVAL;
        }
    }

    if (!WiFiSM.isConnected()) return;

    if ((syncFTPTime != 0) && (currentTime >= syncFTPTime))
    {
        trySyncFTP(nullptr); // Start async FTP
        syncFTPTime = 0;
    }
    else
    {
        if (FTPClient.runAsync())
        {
            if (FTPClient.isAsyncSuccess())
            {
                WiFiSM.logEvent("FTP sync");
                lastFTPSyncTime = currentTime;
            }
            else
            {
                WiFiSM.logEvent("FTP sync failed: %s", FTPClient.getLastError());
                syncFTPTime = currentTime + FTP_RETRY_INTERVAL;
            }
            FTPClient.endAsync();
        }
    }
}


// Boot code
void setup() 
{
    OTGW_SERIAL_INIT;

#ifdef DEBUG_ESP_PORT
    DEBUG_ESP_PORT.setDebugOutput(true);
    DEBUG_ESP_PORT.println("");
    Tracer::traceTo(DEBUG_ESP_PORT);
    Tracer::traceFreeHeap();
#endif

    BuiltinLED.begin();
    EventLog.begin(true);

    OTGW.onMessageReceived(onMessageReceived);
    OTGW.begin(OTGW_RESPONSE_TIMEOUT_MS, OTGW_SETPOINT_OVERRIDE_TIMEOUT);

    PersistentData.begin();
    TimeServer.begin(PersistentData.ntpServer);
    Html.setTitlePrefix(PersistentData.hostName);
    initBoilerLevels();

    resetOtgwTraffic();

    Nav.width = F("12em");
    Nav.menuItems =
    {
        MenuItem 
        {
            .icon = Files[FileId::Home],
            .label = PSTR("Home"),
            .handler = handleHttpRootRequest            
        },
        MenuItem
        {
            .icon = Files[FileId::LogFile],
            .label = PSTR("Event log"),
            .urlPath = PSTR("events"),
            .handler = handleHttpEventLogRequest            
        },
        MenuItem
        {
            .icon = Files[FileId::Graph],
            .label = PSTR("OpenTherm log"),
            .urlPath = PSTR("otlog"),
            .handler = handleHttpOpenThermLogRequest            
        },
        MenuItem
        {
            .icon = Files[FileId::Logo],
            .label = PSTR("OpenTherm data"),
            .urlPath = PSTR("ot"),
            .handler = handleHttpOpenThermRequest            
        },
        MenuItem
        {
            .icon = Files[FileId::Upload],
            .label = PSTR("FTP Sync"),
            .urlPath = PSTR("sync"),
            .handler = handleHttpFTPSyncRequest            
        },
        MenuItem
        {
            .icon = Files[FileId::Calibrate],
            .label = PSTR("OTGW Command"),
            .urlPath = PSTR("cmd"),
            .handler = handleHttpCommandFormRequest,
            .postHandler = handleHttpCommandFormPost            
        },
        MenuItem
        {
            .icon = Files[FileId::Settings],
            .label = PSTR("Settings"),
            .urlPath = PSTR("config"),
            .handler = handleHttpConfigFormRequest,
            .postHandler = handleHttpConfigFormPost            
        }
    };

    Nav.registerHttpHandlers(WebServer);
    WebServer.on("/pump", handleHttpPumpRequest);
    WebServer.on("/traffic", handleHttpOpenThermTrafficRequest);
    WebServer.on("/log-otgw", handleHttpOTGWMessageLogRequest);

    WiFiSM.registerStaticFiles(Files, FileId::_Last);
    WiFiSM.on(WiFiInitState::TimeServerInitializing, onTimeServerInit);
    WiFiSM.on(WiFiInitState::TimeServerSynced, onTimeServerSynced);
    WiFiSM.on(WiFiInitState::Initialized, onWiFiInitialized);
    WiFiSM.on(WiFiInitState::Updating, onWiFiUpdating);
    WiFiSM.scanAccessPoints();
    WiFiSM.begin(PersistentData.wifiSSID, PersistentData.wifiKey, PersistentData.hostName);

    if (PersistentData.heatmonHost[0] != 0)
    {
        HeatMon.begin(PersistentData.heatmonHost);
    }
    if (PersistentData.weatherApiKey[0] != 0)
    {
        WeatherService.begin(PersistentData.weatherApiKey, PersistentData.weatherLocation);
    }

    Tracer::traceFreeHeap();

    BuiltinLED.setOff();
}


// Called repeatedly
void loop() 
{
    currentTime = WiFiSM.getCurrentTime();

    WiFiSM.run();
    if (!OTGW.run(currentTime))
        WiFiSM.logEvent("OTGW error: %s", OTGW.getLastError());

    if (currentTime >= otgwTimeout)
    {
        WiFiSM.logEvent(F("OTGW Timeout"));
        resetOpenThermGateway();
        return;
    }

    if ((otgwInitializeTime != 0) && (currentTime >= otgwInitializeTime))
    {
        otgwInitializeTime = 0;
        initializeOpenThermGateway();
        return;
    }

    // Scheduled Boiler TSet change (incl. forced PWM)
    if ((changeBoilerLevelTime != 0) && (currentTime >= changeBoilerLevelTime))
    {
        changeBoilerLevelTime = 0;
        if (pwmDutyCycle < 1)
        {
            if (currentBoilerLevel == BoilerLevel::Off)
                setBoilerLevel(BoilerLevel::Low, pwmDutyCycle * PWM_PERIOD);
            else if (currentBoilerLevel == BoilerLevel::Low)
                setBoilerLevel(BoilerLevel::Off, (1.0F - pwmDutyCycle) * PWM_PERIOD);
            else
            {
                WiFiSM.logEvent(F("Unexpected level for PWM: %s"), BoilerLevelNames[currentBoilerLevel]);
                cancelOverride();
            }
        }
        else if (changeBoilerLevel == BoilerLevel::Thermostat)
            cancelOverride();
        else
        {
            WiFiSM.logEvent(
                F("Override %s changed to %s"),
                BoilerLevelNames[currentBoilerLevel],
                BoilerLevelNames[changeBoilerLevel]);
            setBoilerLevel(changeBoilerLevel, TSET_OVERRIDE_DURATION);
        }
        return;
    }

    if (updateTOutside)
    {
        updateTOutside = false;
        setOtgwResponse(OpenThermDataId::TOutside, WeatherService.temperature);
        return;
    }

    if (updateHeatmonData)
    {
        updateHeatmonData = false;
        setOtgwResponse(OpenThermDataId::TReturn, HeatMon.tOut);
        setOtgwResponse(OpenThermDataId::Tdhw, HeatMon.tBuffer);
        return;
    }
}
