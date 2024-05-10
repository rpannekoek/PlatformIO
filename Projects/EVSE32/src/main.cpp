#include <Arduino.h>
#include <math.h>
#include <ESPWiFi.h>
#include <ESPWebServer.h>
#include <ESPFileSystem.h>
#include <WiFiStateMachine.h>
#include <WiFiNTP.h>
#include <WiFiFTP.h>
#include <Ticker.h>
#include <TimeUtils.h>
#include <Tracer.h>
#include <StringBuilder.h>
#include <Navigation.h>
#include <HtmlWriter.h>
#include <LED.h>
#include <Log.h>
#include <BLE.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include "PersistentData.h"
#include "CurrentSensor.h"
#include "VoltageSensor.h"
#include "IEC61851ControlPilot.h"
#include "Status.h"
#include "DsmrMonitorClient.h"
#include "DayStatistics.h"
#include "ChargeLogEntry.h"
#include "ChargeStatsEntry.h"

constexpr int FTP_RETRY_INTERVAL = 15 * SECONDS_PER_MINUTE;
constexpr int HTTP_POLL_INTERVAL = 60;
constexpr int TEMP_POLL_INTERVAL = 10;
constexpr int CHARGE_CONTROL_INTERVAL = 10;
constexpr int CHARGE_LOG_AGGREGATIONS = 6;
constexpr int CHARGE_STATS_SIZE = 5;
constexpr int CHARGE_LOG_SIZE = 200;
constexpr int CHARGE_LOG_PAGE_SIZE = 50;
constexpr int EVENT_LOG_LENGTH = 50;

#ifdef ARDUINO_LOLIN_S3_MINI
constexpr uint8_t RELAY_START_PIN = 13;
constexpr uint8_t RELAY_ON_PIN = 11;
constexpr uint8_t CURRENT_SENSE_PIN = 4;
constexpr uint8_t VOLTAGE_SENSE_PIN = 2;
constexpr uint8_t EXTERNAL_RGBLED_PIN = 35;
constexpr uint8_t CP_OUTPUT_PIN = 18;
constexpr uint8_t CP_INPUT_PIN = 10;
constexpr uint8_t CP_FEEDBACK_PIN = 16;
constexpr uint8_t TEMP_SENSOR_PIN = 12;

#ifdef DEBUG_ESP_PORT
constexpr uint8_t STATUS_LED_PIN = RGB_BUILTIN;
#else
constexpr uint8_t STATUS_LED_PIN = EXTERNAL_RGBLED_PIN;
#endif

#else
constexpr uint8_t RELAY_START_PIN = 12;
constexpr uint8_t RELAY_ON_PIN = 13;
constexpr uint8_t CURRENT_SENSE_PIN = 34;
constexpr uint8_t VOLTAGE_SENSE_PIN = 32;
constexpr uint8_t EXTERNAL_RGBLED_PIN = 17;
constexpr uint8_t CP_OUTPUT_PIN = 15;
constexpr uint8_t CP_INPUT_PIN = 33;
constexpr uint8_t CP_FEEDBACK_PIN = 16;
constexpr uint8_t TEMP_SENSOR_PIN = 14;
constexpr uint8_t STATUS_LED_PIN = EXTERNAL_RGBLED_PIN;
#endif


constexpr float ZERO_CURRENT_THRESHOLD = 0.2;
constexpr float LOW_CURRENT_THRESHOLD = 0.75;
constexpr float CHARGE_VOLTAGE = 230;

#define CAL_CURRENT "ActualCurrent"
#define CAL_CURRENT_ZERO "CurrentZero"
#define CAL_TEMP_OFFSET "TempOffset"

enum FileId
{
    Logo,
    Styles,
    BluetoothIcon,
    CalibrateIcon,
    CancelIcon,
    ConfirmIcon,
    FlashIcon,
    HomeIcon,
    LogFileIcon,
    MeterIcon,
    SettingsIcon,
    UploadIcon,
    _LastFileId
};

const char* Files[] PROGMEM =
{
    "Logo.png",
    "styles.css",
    "Bluetooth.svg",
    "Calibrate.svg",
    "Cancel.svg",
    "Confirm.svg",
    "Flash.svg",
    "Home.svg",
    "LogFile.svg",
    "Meter.svg",
    "Settings.svg",
    "Upload.svg"
};

const char* ContentTypeHtml = "text/html;charset=UTF-8";
const char* ContentTypeJson = "application/json";
const char* ContentTypeText = "text/plain";

const char* ButtonClass = "button";

ESPWebServer WebServer(80); // Default HTTP port
WiFiNTP TimeServer;
WiFiFTPClient FTPClient(2000); // 2s timeout
DsmrMonitorClient SmartMeter(5000); // 5s timeout
BLE Bluetooth;
StringBuilder HttpResponse(8192); // 8KB HTTP response buffer
HtmlWriter Html(HttpResponse, Files[Logo], Files[Styles], 60);
Log<const char> EventLog(EVENT_LOG_LENGTH);
StatusLED StateLED(STATUS_LED_PIN);
WiFiStateMachine WiFiSM(StateLED, TimeServer, WebServer, EventLog);
CurrentSensor OutputCurrentSensor(CURRENT_SENSE_PIN);
VoltageSensor OutputVoltageSensor(VOLTAGE_SENSE_PIN);
IEC61851ControlPilot ControlPilot(CP_OUTPUT_PIN, CP_INPUT_PIN, CP_FEEDBACK_PIN);
OneWire OneWireBus(TEMP_SENSOR_PIN);
DallasTemperature TempSensors(&OneWireBus);
StaticLog<ChargeLogEntry> ChargeLog(CHARGE_LOG_SIZE);
StaticLog<ChargeStatsEntry> ChargeStats(CHARGE_STATS_SIZE);
DayStatistics DayStats;
Navigation Nav;

EVSEState state = EVSEState::Booting;
float temperature = 0;
float outputCurrent = 0;
float currentLimit = 0;
int aggregations = 0;
bool isRelayActivated = false;
bool isWebAuthorized = false;
bool isMeasuringTemp = false;
bool ftpSyncChargeStats = false;

time_t currentTime = 0;
time_t stateChangeTime = 0;
time_t tempPollTime = 0;
time_t chargeControlTime = 0;
time_t ftpSyncTime = 0;
time_t lastFTPSyncTime = 0;

int logEntriesToSync = 0;
ChargeLogEntry newChargeLogEntry;
ChargeLogEntry* lastChargeLogEntryPtr = nullptr;

ChargeStatsEntry* lastChargeStatsPtr = nullptr;

// Forward defines
void handleHttpRootRequest();
void handleHttpEventLogRequest();
void handleHttpChargeLogRequest();
void handleHttpSyncFTPRequest();
void handleHttpBluetoothRequest();
void handleHttpBluetoothFormPost();
void handleHttpSmartMeterRequest();
void handleHttpCalibrateRequest();
void handleHttpConfigFormRequest();
void handleHttpConfigFormPost();
void handleHttpBluetoothJsonRequest();
void handleHttpCurrentRequest();
void onWiFiTimeSynced();
void onWiFiInitialized();
bool setRelay(bool);

 
void setState(EVSEState newState)
{
    state = newState;
    stateChangeTime = currentTime;
    WiFiSM.logEvent("EVSE State changed to %s", EVSEStateNames[newState]);
    if (!StateLED.setStatus(newState))
        WiFiSM.logEvent("Failed setting RGB LED status");
    delay(10);
}


void setFailure(const String& reason)
{
    WiFiSM.logEvent(reason);
    if (state != EVSEState::Booting)
    {
        ControlPilot.setOff();
        if (isRelayActivated)
        {
            delay(500);
            setRelay(false);
        } 
    }
    setState(EVSEState::Failure);
}


void setUnexpectedControlPilotStatus()
{
    WiFiSM.logEvent("Control Pilot: %0.1f V", ControlPilot.getVoltage());
    String message = "Unexpected Control Pilot status: ";
    message += ControlPilot.getStatusName();
    setFailure(message); 
}


bool setRelay(bool on)
{
    const char* relayState = on ? "on" : "off";
    Tracer tracer(F(__func__), relayState);

    isRelayActivated = on;

    if (on)
    {
        digitalWrite(RELAY_START_PIN, 1);
        delay(500);
        digitalWrite(RELAY_ON_PIN, 1);
        digitalWrite(RELAY_START_PIN, 0);
    }
    else
    {
        digitalWrite(RELAY_ON_PIN, 0);
        digitalWrite(RELAY_START_PIN, 0);
        delay(100);
    }

    if (OutputVoltageSensor.detectSignal() != isRelayActivated)
    {
        if (state != EVSEState::Failure)
        {
            String message = "Failed setting relay ";
            message += relayState; 
            setFailure(message);
        }
        return false;
    }

    setCpuFrequencyMhz(on ? 240 : 80);

    WiFiSM.logEvent("Relay set %s. CPU @ %d MHz", relayState, getCpuFrequencyMhz());
    return true;
}


bool initTempSensor()
{
    Tracer tracer(F(__func__));

    TempSensors.begin();
    TempSensors.setWaitForConversion(false);

    TRACE("Found %d OneWire devices.\n", TempSensors.getDeviceCount());
    TRACE("Found %d temperature sensors.\n", TempSensors.getDS18Count());

    if (TempSensors.getDS18Count() > 0 && !TempSensors.validFamily(PersistentData.tempSensorAddress))
    {
        bool newSensorFound = TempSensors.getAddress(PersistentData.tempSensorAddress, 0);
        if (newSensorFound)
            PersistentData.writeToEEPROM();
        else
        {
            setFailure("Unable to obtain temperature sensor address.");
            return false;
        }
    }

    DeviceAddress& addr = PersistentData.tempSensorAddress;
    if (!TempSensors.isConnected(addr))
    {
        setFailure("Temperature sensor is not connected");
        memset(PersistentData.tempSensorAddress, 0, sizeof(DeviceAddress));
        PersistentData.writeToEEPROM();
        return false;
    }

    WiFiSM.logEvent(
        "Temperature sensor address: %02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X. Offset: %0.2f",
        addr[0], addr[1], addr[2], addr[3], addr[4], addr[5], addr[6], addr[7],
        PersistentData.tempSensorOffset);

    return true;
}


// Boot code
void setup()
{
    Serial.begin(115200); // Use same baudrate as bootloader
    Serial.setTimeout(1000);
    Serial.println();

    #ifdef DEBUG_ESP_PORT
    Tracer::traceTo(DEBUG_ESP_PORT);
    Tracer::traceFreeHeap();
    #endif

    if (!StateLED.begin())
        setFailure("Failed initializing RGB LED");

    PersistentData.begin();
    TimeServer.begin(PersistentData.ntpServer);
    Html.setTitlePrefix(PersistentData.hostName);

    Nav.width = "8em";
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
            .urlPath = PSTR("events"),
            .handler = handleHttpEventLogRequest
        },
        MenuItem
        {
            .icon = Files[FlashIcon],
            .label = PSTR("Charge log"),
            .urlPath = PSTR("chargelog"),
            .handler = handleHttpChargeLogRequest
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
            .icon = Files[BluetoothIcon],
            .label = PSTR("Bluetooth"),
            .urlPath = PSTR("bt"),
            .handler = handleHttpBluetoothRequest,
            .postHandler = handleHttpBluetoothFormPost
        },
        MenuItem
        {
            .icon = Files[MeterIcon],
            .label = PSTR("Smart Meter"),
            .urlPath = PSTR("dsmr"),
            .handler = handleHttpSmartMeterRequest
        },
        MenuItem
        {
            .icon = Files[CalibrateIcon],
            .label = PSTR("Calibrate"),
            .urlPath = PSTR("calibrate"),
            .handler = handleHttpCalibrateRequest
        },
        MenuItem
        {
            .icon = Files[SettingsIcon],
            .label = PSTR("Settings"),
            .urlPath = PSTR("config"),
            .handler = handleHttpConfigFormRequest,
            .postHandler = handleHttpConfigFormPost
        },
    };
    Nav.registerHttpHandlers(WebServer);

    WebServer.on("/bt/json", handleHttpBluetoothJsonRequest);
    WebServer.on("/current", handleHttpCurrentRequest);
    
    WiFiSM.registerStaticFiles(Files, _LastFileId);
    WiFiSM.on(WiFiInitState::TimeServerSynced, onWiFiTimeSynced);
    WiFiSM.on(WiFiInitState::Initialized, onWiFiInitialized);
    WiFiSM.scanAccessPoints();
    WiFiSM.begin(PersistentData.wifiSSID, PersistentData.wifiKey, PersistentData.hostName);

    if (!OutputCurrentSensor.begin(PersistentData.currentZero, PersistentData.currentScale))
        setFailure("Failed initializing current sensor");

    if (!OutputVoltageSensor.begin())
        setFailure("Failed initializing voltage sensor");

    pinMode(RELAY_START_PIN, OUTPUT);
    pinMode(RELAY_ON_PIN, OUTPUT);
    setRelay(false);

    if (!ControlPilot.begin())
        setFailure("Failed initializing Control Pilot");

    if (Bluetooth.begin(PersistentData.hostName))
        Bluetooth.registerBeacons(PersistentData.registeredBeaconCount, PersistentData.registeredBeacons);
    else
        setFailure("Failed initializing Bluetooth");

    if (PersistentData.dsmrMonitor[0] != 0)
    {
        if (!SmartMeter.begin(PersistentData.dsmrMonitor))
            setFailure("Failed initializing Smart Meter");
    }

    initTempSensor();

    Tracer::traceFreeHeap();
}


bool isChargingAuthorized()
{
    if (isWebAuthorized)
    {
        WiFiSM.logEvent("Charging authorized through web");
        return true;
    }

    if (Bluetooth.isDeviceDetected())
    {
        WiFiSM.logEvent("Charging authorized through Bluetooth");
        return true;
    } 

    if ((PersistentData.authorizeTimeout != 0) && (currentTime - stateChangeTime > PersistentData.authorizeTimeout))
    {
        WiFiSM.logEvent(
            "Charging authorized by timeout (%s)",
            formatTimeSpan(PersistentData.authorizeTimeout));
        return true;
    }

    // If discovery complete, start new one.
    if (Bluetooth.getState() == BluetoothState::DiscoveryComplete)
        Bluetooth.startDiscovery();

    return false;
}


bool stopCharging(const char* cause)
{
    Tracer tracer(F(__func__), cause);

    WiFiSM.logEvent("Charging stopped by %s.", cause);

    ControlPilot.setOff();
    ControlPilot.awaitStatus(ControlPilotStatus::NoPower);

    // Wait max 5 seconds for output current to drop below threshold
    int timeout = 50;
    do
    {
        OutputCurrentSensor.measure();
        outputCurrent = OutputCurrentSensor.getRMS();
    }
    while (outputCurrent > LOW_CURRENT_THRESHOLD && --timeout > 0);
    if (timeout == 0)
        WiFiSM.logEvent("Vehicle keeps drawing current: %0.1f A", outputCurrent);

    if (!setRelay(false)) return false;    

    ControlPilot.setReady();
    ControlPilot.awaitStatus(ControlPilotStatus::VehicleDetected);

    switch (ControlPilot.getStatus())
    {
        case ControlPilotStatus::Standby:
            setState(EVSEState::Ready);
            break;

        case ControlPilotStatus::VehicleDetected:
            setState(EVSEState::ChargeCompleted);
            break;

        default:
            setState(EVSEState::StopCharging);
    }

    lastChargeStatsPtr->update(currentTime, outputCurrent * CHARGE_VOLTAGE, temperature);
    if (PersistentData.isFTPEnabled())
    {
        ftpSyncTime = currentTime;
        ftpSyncChargeStats = true;
    }

    return true;
}


float getDeratedCurrentLimit()
{
    float result = PersistentData.currentLimit;
    if (temperature > PersistentData.tempLimit)
    {
        // Derate current when above temperature limit: 1 A/degree.
        result = std::min(result, 16.0F) - (temperature - PersistentData.tempLimit);
    }
    return result;
}


float determineCurrentLimit()
{
    float deratedCurrentLimit = getDeratedCurrentLimit();

    if (!SmartMeter.isInitialized)
        return deratedCurrentLimit;

    if (!WiFiSM.isConnected())
        return (temperature > PersistentData.tempLimit) ? deratedCurrentLimit : 0;

    if (SmartMeter.requestData() != HTTP_CODE_OK)
    {
        WiFiSM.logEvent("Smart Meter: %s", SmartMeter.getLastError().c_str());
        return (temperature > PersistentData.tempLimit) ? deratedCurrentLimit : 0;
    }

    PhaseData& phase = SmartMeter.getElectricity()[PersistentData.dsmrPhase - 1]; 
    float phaseCurrent = phase.Pdelivered / phase.U; 
    if (state == EVSEState::Charging)
        phaseCurrent = std::max(phaseCurrent - outputCurrent, 0.0F);

    return std::min((float)PersistentData.currentLimit - phaseCurrent, deratedCurrentLimit);
}


void chargeControl()
{
    Tracer tracer(F(__func__));

    if (!OutputVoltageSensor.detectSignal())
    {
        setFailure("Output voltage lost");
        return;        
    }

    OutputCurrentSensor.measure(); 
    if (OutputCurrentSensor.getSampleCount() > 50)
    {
        outputCurrent = OutputCurrentSensor.getRMS();
        if (outputCurrent > currentLimit * 1.25)
        {
            setFailure("Output current too high");
            return;
        }
        float cl = determineCurrentLimit();
        if (cl > 0) currentLimit = ControlPilot.setCurrentLimit(cl);
    }
    else
        WiFiSM.logEvent("Insufficient current samples: %d", OutputCurrentSensor.getSampleCount());

    lastChargeStatsPtr->update(currentTime, outputCurrent * CHARGE_VOLTAGE, temperature);

    newChargeLogEntry.update(currentLimit, outputCurrent, temperature);
    if (++aggregations == CHARGE_LOG_AGGREGATIONS)
    {
        newChargeLogEntry.average(aggregations);
        if (lastChargeLogEntryPtr == nullptr || !newChargeLogEntry.equals(lastChargeLogEntryPtr))
        {
            lastChargeLogEntryPtr = ChargeLog.add(&newChargeLogEntry);

            logEntriesToSync = std::min(logEntriesToSync + 1, CHARGE_LOG_SIZE);
            if (PersistentData.isFTPEnabled() && (logEntriesToSync == PersistentData.ftpSyncEntries))
                ftpSyncTime = currentTime;
        }
        newChargeLogEntry.reset(currentTime);
        aggregations = 0;
    }
}


bool selfTest()
{
    Tracer tracer(F(__func__));

    int cpStandbyLevel = ControlPilot.calibrate();
    WiFiSM.logEvent("Control Pilot standby level: %d", cpStandbyLevel);
    if (cpStandbyLevel < MIN_CP_STANDBY_LEVEL)
    {
        WiFiSM.logEvent("Control Pilot standby level too low");
        return false;
    }

    ControlPilot.setOff();
    if (!ControlPilot.awaitStatus(ControlPilotStatus::NoPower))
    {
        WiFiSM.logEvent("Control Pilot off: %0.1f V", ControlPilot.getVoltage());
        return false;
    }

    if (OutputVoltageSensor.detectSignal())
    {
        WiFiSM.logEvent("Output voltage present before relay activation");
        return false;
    }

    OutputCurrentSensor.measure();
    float outputCurrent = OutputCurrentSensor.getRMS(); 
    if (outputCurrent > ZERO_CURRENT_THRESHOLD)
    {
        WiFiSM.logEvent("Output current before relay activation: %0.2f A", outputCurrent);
        return false;
    }

    if (!setRelay(true)) return false;

    OutputCurrentSensor.measure();
    outputCurrent = OutputCurrentSensor.getRMS(); 
    if (outputCurrent > ZERO_CURRENT_THRESHOLD)
    {
        WiFiSM.logEvent("Output current after relay activation: %0.2f A", outputCurrent);
        setRelay(false);
        return false;
    }

    if (!setRelay(false)) return false;

    OutputCurrentSensor.measure();
    outputCurrent = OutputCurrentSensor.getRMS(); 
    if (outputCurrent > ZERO_CURRENT_THRESHOLD)
    {
        WiFiSM.logEvent("Output current after relay deactivation: %0.2f A", outputCurrent);
        return false;
    }

    ControlPilot.setReady();
    if (!ControlPilot.awaitStatus(ControlPilotStatus::Standby))
    {
        WiFiSM.logEvent("Control Pilot standby: %0.1f V\n", ControlPilot.getVoltage());
        return false;
    }

    return true;
}


void runEVSEStateMachine()
{
    ControlPilotStatus cpStatus = ControlPilot.getStatus();

    switch (state)
    {
        case EVSEState::SelfTest:
            if (selfTest())
            {
                WiFiSM.logEvent("Self-test passed");
                setState(EVSEState::Ready);
            }
            else
                setFailure("Self-test failed");
            break;

        case EVSEState::Ready: // Await vehicle connection
            if (cpStatus == ControlPilotStatus::VehicleDetected)
            {
                isWebAuthorized = false;
                if (Bluetooth.startDiscovery())
                    setState(EVSEState::Authorize);
                else
                    setFailure("Bluetooth discovery failed");
            }
            else if (cpStatus != ControlPilotStatus::Standby)
                setUnexpectedControlPilotStatus();
            break;

        case EVSEState::Authorize: // Await authorization
            if (isChargingAuthorized())
            {
                if (setRelay(true))
                {
                    currentLimit = ControlPilot.setCurrentLimit(determineCurrentLimit());
                    setState(EVSEState::AwaitCharging);
                }
            }
            else if (cpStatus == ControlPilotStatus::Standby)
                setState(EVSEState::Ready);
            else if (cpStatus != ControlPilotStatus::VehicleDetected)
                setUnexpectedControlPilotStatus();
            break;

        case EVSEState::AwaitCharging: // Authorized; Wait till vehicle starts charging
            if (cpStatus == ControlPilotStatus::Charging || cpStatus == ControlPilotStatus::ChargingVentilated)
            {
                ChargeStatsEntry newChargeStats;
                newChargeStats.init(currentTime);
                lastChargeStatsPtr = ChargeStats.add(&newChargeStats);
                newChargeLogEntry.reset(currentTime);
                ChargeLog.clear();
                aggregations = 0;
                chargeControlTime = currentTime + CHARGE_CONTROL_INTERVAL;
                setState(EVSEState::Charging);
            }
            else if (cpStatus == ControlPilotStatus::Standby)
            {
                if (setRelay(false))
                {
                    ControlPilot.setReady();
                    setState(EVSEState::Ready);
                }
            }
            else if ((currentTime - stateChangeTime) > 60)
                setFailure("Timeout waiting for charging to start");
            break;

        case EVSEState::Charging:
            if (cpStatus == ControlPilotStatus::VehicleDetected || cpStatus == ControlPilotStatus::Standby)
                stopCharging("vehicle");
            else if (currentTime >= chargeControlTime)
            {
                chargeControlTime += CHARGE_CONTROL_INTERVAL;
                chargeControl();
            }
            break;

        case EVSEState::StopCharging:
            if (cpStatus == ControlPilotStatus::VehicleDetected)
                setState(EVSEState::ChargeCompleted);
            else if (cpStatus == ControlPilotStatus::Standby)
                setState(EVSEState::Ready);
            else if (cpStatus != ControlPilotStatus::Charging && cpStatus != ControlPilotStatus::ChargingVentilated)
                setUnexpectedControlPilotStatus();
            break;

        case EVSEState::ChargeCompleted:
            if (cpStatus == ControlPilotStatus::Standby)
                setState(EVSEState::Ready);
            else if (cpStatus != ControlPilotStatus::VehicleDetected)
                setUnexpectedControlPilotStatus();
            break;

        case EVSEState::Booting:
        case EVSEState::Failure:
            break;
    }
}


void test(String message)
{
    Tracer tracer(F(__func__), message.c_str());

    if (message.startsWith("testF"))
    {
        for (int i = 0; i < EVENT_LOG_LENGTH; i++)
            WiFiSM.logEvent("Test entry to fill up the event log.");

        for (int i = 0; i < CHARGE_LOG_SIZE; i++)
        {
            newChargeLogEntry.time = currentTime + i * 60;
            newChargeLogEntry.currentLimit = i % 16;
            newChargeLogEntry.outputCurrent = i % 16;
            newChargeLogEntry.temperature = i % 20 + 10;
            lastChargeLogEntryPtr = ChargeLog.add(&newChargeLogEntry);
        }
        logEntriesToSync = 3;

        for (int i = 0; i < CHARGE_STATS_SIZE; i++)
        {
            time_t startTime = currentTime + i * SECONDS_PER_DAY;
            ChargeStatsEntry newChargeStats;
            newChargeStats.init(startTime);
            newChargeStats.update(startTime + (i * SECONDS_PER_HOUR), i * 100, i + 40);
            lastChargeStatsPtr = ChargeStats.add(&newChargeStats);
        }
        ftpSyncChargeStats = true;
    }
    else if (message.startsWith("testB"))
    {
        StateLED.setStatus(EVSEState::Ready); // Color = Breathing Green
    }
    else if (message.startsWith("testL"))
    {
        for (int j = 0; j < 8; j++)
        {
            StateLED.setStatus(static_cast<EVSEState>(j));
            delay(2000);
        }
    }
    else if (message.startsWith("testR"))
    {
        for (int i = 0; i < 10; i++)
        {
            setRelay(i % 2 == 0);
            delay(5000);
        }
    }
    else if (message.startsWith("testV"))
    {
        if (OutputVoltageSensor.detectSignal(100))
        {
            TRACE("Output voltage detected.\n");
        }
    }
    else if (message.startsWith("testC"))
    {
        TRACE("CP Off Voltage: %0.2f V\n", ControlPilot.getVoltage());

        ControlPilot.setReady();
        delay(10);
        TRACE("CP Idle Voltage: %0.2f V\n", ControlPilot.getVoltage());
        delay(1000);

        for (int i = 20; i >= 5; i--)
        {
            ControlPilot.setCurrentLimit(i);
            delay(500);
            TRACE("CP Voltage @ %d A: %0.2f V\n", i, ControlPilot.getVoltage());
            delay(1500);
        }

        ControlPilot.setReady();
        setState(EVSEState::Ready);
    }
    else if (message.startsWith("s"))
    {
        int number = message.substring(1).toInt();
        setState(static_cast<EVSEState>(number));
    }
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

    runEVSEStateMachine();
}


void writeCsvChargeStatsEntry(ChargeStatsEntry* chargeStatsPtr, Print& destination)
{
    destination.printf(
        "%s;%0.1f;%0.1f;%0.1f;%0.1f\r\n",
        formatTime("%F %H:%M", chargeStatsPtr->startTime),
        chargeStatsPtr->getDurationHours(),
        chargeStatsPtr->getAvgTemperature(),
        chargeStatsPtr->getAvgPower() / 1000,
        chargeStatsPtr->energy / 1000);
}


void writeCsvChangeLogEntries(ChargeLogEntry* logEntryPtr, Print& destination)
{
    while (logEntryPtr != nullptr)
    {
        destination.printf(
            "%s;%0.1f;%0.1f;%0.1f\r\n",
            formatTime("%F %H:%M:%S", logEntryPtr->time),
            logEntryPtr->currentLimit,
            logEntryPtr->outputCurrent,
            logEntryPtr->temperature);

        logEntryPtr = ChargeLog.getNextEntry();
    }
}


bool trySyncFTP(Print* printTo)
{
    Tracer tracer(F(__func__));

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
        if (logEntriesToSync > 0)
        {
            ChargeLogEntry* firstLogEntryPtr = ChargeLog.getEntryFromEnd(logEntriesToSync);
            writeCsvChangeLogEntries(firstLogEntryPtr, dataClient);
            logEntriesToSync = 0;
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

    if (ftpSyncChargeStats)
    {
        snprintf(filename, sizeof(filename), "%s_stats.csv", PersistentData.hostName);
        if (FTPClient.passive())
        {
            WiFiClient& dataClient = FTPClient.append(filename);
            if (dataClient.connected())
            {
                writeCsvChargeStatsEntry(lastChargeStatsPtr, dataClient);
                dataClient.stop();
            }

            if (FTPClient.readServerResponse() == 226)
                ftpSyncChargeStats = false;
            else
            {
                success = false;
                FTPClient.setUnexpectedResponse();
            }
        }
        else
            success = false;
    }

    FTPClient.end();

    return success;
}


void onWiFiTimeSynced()
{
    if (state == EVSEState::Booting)
        setState(EVSEState::SelfTest);
    tempPollTime = currentTime;
}


void onWiFiInitialized()
{
    if ((state == EVSEState::Failure) && !StateLED.isOn())
        StateLED.setOn(true);

    if (currentTime >= tempPollTime && TempSensors.getDS18Count() > 0)
    {
        isMeasuringTemp = true;
        tempPollTime = currentTime + TEMP_POLL_INTERVAL;
        TempSensors.requestTemperatures();
    }

    if (isMeasuringTemp && TempSensors.isConversionComplete())
    {
        float tMeasured = TempSensors.getTempC(PersistentData.tempSensorAddress);
        if (tMeasured == DEVICE_DISCONNECTED_C)
        {
            if (state != EVSEState::Failure)
                setFailure("Temperature sensor disconnected");
        }
        else if (tMeasured == 85)
            WiFiSM.logEvent("Invalid temperature sensor reading");
        else
        {
            temperature = tMeasured + PersistentData.tempSensorOffset;
            DayStats.update(currentTime, temperature);
            if (temperature > (PersistentData.tempLimit + 10) && state != EVSEState::Failure)
                setFailure("Temperature too high");
        }
        
        isMeasuringTemp = false;
    }

    if ((ftpSyncTime != 0) && (currentTime >= ftpSyncTime) && WiFiSM.isConnected())
    {
        if (trySyncFTP(nullptr))
        {
            WiFiSM.logEvent("FTP sync");
            ftpSyncTime = 0;
        }
        else
        {
            WiFiSM.logEvent("FTP sync failed: %s", FTPClient.getLastError());
            ftpSyncTime = currentTime + FTP_RETRY_INTERVAL;
        }
    }
}


void writeChargeStatistics()
{
    Html.writeHeading("Charging sessions");

    Html.writeTableStart();
    Html.writeRowStart();
    Html.writeHeaderCell("Start");
    Html.writeHeaderCell("Hours");
    Html.writeHeaderCell("T (°C)");
    Html.writeHeaderCell("P (kW)");
    Html.writeHeaderCell("E (kWh)");
    Html.writeRowEnd();

    ChargeStatsEntry* chargeStatsPtr = ChargeStats.getFirstEntry();
    while (chargeStatsPtr != nullptr)
    {
        Html.writeRowStart();
        Html.writeCell(formatTime("%d %b %H:%M", chargeStatsPtr->startTime));
        Html.writeCell(chargeStatsPtr->getDurationHours(), F("%0.1f"));
        Html.writeCell(chargeStatsPtr->getAvgTemperature(), F("%0.1f"));
        Html.writeCell(chargeStatsPtr->getAvgPower() / 1000, F("%0.1f"));
        Html.writeCell(chargeStatsPtr->energy / 1000, F("%0.1f"));
        Html.writeRowEnd();

        chargeStatsPtr = ChargeStats.getNextEntry();
    }

    Html.writeTableEnd();
}


void handleHttpRootRequest()
{
    Tracer tracer(F(__func__));

    if (WiFiSM.isInAccessPointMode())
    {
        handleHttpConfigFormRequest();
        return;
    }

    Html.writeHeader("Home", Nav, HTTP_POLL_INTERVAL);

    Html.writeTableStart();

    Html.writeRow(
        "EVSE State",
        "<span style=\"color: %s; font-weight: bold\">%s</span>",
        EVSEStateColors[state],
        EVSEStateNames[state]);
    Html.writeRow("Control Pilot", "%s", ControlPilot.getStatusName());

    if (state == EVSEState::Charging)
    {
        Html.writeRow("Current limit", "%0.1f A", currentLimit);
        Html.writeRow("Output current", "%0.1f A", outputCurrent);
    }

    Html.writeRow("Temperature", "%0.1f °C", temperature);
    Html.writeRow("T<sub>max</sub>", "%0.1f °C @ %s", DayStats.tMax, formatTime("%H:%M", DayStats.tMaxTime));
    Html.writeRow("T<sub>min</sub>", "%0.1f °C @ %s", DayStats.tMin, formatTime("%H:%M", DayStats.tMinTime));

    Html.writeRow("WiFi RSSI", "%d dBm", static_cast<int>(WiFi.RSSI()));
    Html.writeRow("WiFi AP", "%s", WiFi.BSSIDstr().c_str());
    Html.writeRow("Free Heap", "%0.1f kB", float(ESP.getFreeHeap()) / 1024);
    Html.writeRow("Uptime", "%0.1f days", float(WiFiSM.getUptime()) / SECONDS_PER_DAY);

    String ftpSync;
    if (!PersistentData.isFTPEnabled())
        ftpSync = "Disabled";
    else if (lastFTPSyncTime == 0)
        ftpSync = "Not yet";
    else
        ftpSync = formatTime("%H:%M", lastFTPSyncTime);

    Html.writeRow("FTP Sync", ftpSync);
    if (PersistentData.isFTPEnabled())
        Html.writeRow("Sync entries", "%d / %d", logEntriesToSync, PersistentData.ftpSyncEntries);

    Html.writeTableEnd();

    switch (state)
    {
        case EVSEState::Ready:
        case EVSEState::Failure:
            if (WiFiSM.shouldPerformAction("selftest"))
            {
                Html.writeParagraph("Performing self-test...");
                setState(EVSEState::SelfTest);
            }
            else
                Html.writeActionLink("selftest", "Perform self-test", currentTime, ButtonClass, Files[CalibrateIcon]);
            break;

        case EVSEState::Authorize:
            if (WiFiSM.shouldPerformAction("authorize"))
            {
                Html.writeParagraph("Charging authorized.");
                isWebAuthorized = true;
            }
            else if (!isWebAuthorized)
                Html.writeActionLink("authorize", "Start charging", currentTime, ButtonClass, Files[FlashIcon]);
            break;

        case EVSEState::AwaitCharging:
        case EVSEState::Charging:
            if (WiFiSM.shouldPerformAction("stop"))
            {
                if (stopCharging("EVSE"))
                    Html.writeParagraph("Charging stopped.");
                else
                    Html.writeParagraph("Stop charging failed.");
            }
            else
                Html.writeActionLink("stop", "Stop charging", currentTime, ButtonClass, Files[CancelIcon]);
            break;

        default:
            break;
    }

    writeChargeStatistics();

    Html.writeFooter();

    WebServer.send(200, ContentTypeHtml, HttpResponse.c_str());
}


void handleHttpBluetoothRequest()
{
    Tracer tracer(F(__func__));

    BluetoothState btState = Bluetooth.getState();
    uint16_t refreshInterval = (btState == BluetoothState::Discovering) ? 5 : 0;

    Html.writeHeader("Bluetooth", Nav, refreshInterval);

    if (WiFiSM.shouldPerformAction("startDiscovery"))
    {
        if (Bluetooth.startDiscovery())
        {
            handleHttpBluetoothRequest();
            return;
        }
        else
            Html.writeParagraph("Scanning for devices failed.");
    }
    else if (btState == BluetoothState::Initialized || btState == BluetoothState::DiscoveryComplete)
        Html.writeActionLink("startDiscovery", "Scan for devices", currentTime, ButtonClass);

    if (Bluetooth.isDeviceDetected())
        Html.writeParagraph("Registered device detected");

    Html.writeFormStart("/bt");

    Html.writeHeading("Registered beacons", 2);
    for (int i = 0; i < PersistentData.registeredBeaconCount; i++)
    {
        UUID128 uuid = UUID128(PersistentData.registeredBeacons[i]);
        String uuidStr = uuid.toString();
        HttpResponse.printf(
            F("<div><input type=\"checkbox\" name=\"uuid\" value=\"%s\" checked>%s</div>\r\n"), 
            uuidStr.c_str(),
            uuidStr.c_str());
    }

    if (btState == BluetoothState::DiscoveryComplete)
    {
        Html.writeHeading("Discovered beacons", 2);
        Html.writeTableStart();
        Html.writeRowStart();
        Html.writeHeaderCell("");
        Html.writeHeaderCell("Address");
        Html.writeHeaderCell("UUID");
        Html.writeHeaderCell("RSSI");
        Html.writeRowEnd();
        for (BluetoothDeviceInfo& btDeviceInfo : Bluetooth.getDiscoveredDevices())
        {
            if (btDeviceInfo.uuid == nullptr) continue;
            const char* uuid = btDeviceInfo.uuid->toString().c_str();

            Html.writeRowStart();
            HttpResponse.printf(
                F("<td><input type=\"checkbox\" name=\"uuid\" value=\"%s\" %s></td>"), 
                uuid,
                btDeviceInfo.isRegistered ? "checked" : "");
            Html.writeCell(btDeviceInfo.getAddress());
            Html.writeCell(uuid);
            Html.writeCell(btDeviceInfo.rssi);
            Html.writeRowEnd();
        }
        Html.writeTableEnd();
    }
    else if (btState == BluetoothState::Discovering)
        Html.writeParagraph("Discovery in progress...");

    Html.writeSubmitButton("Update registration", ButtonClass);
    Html.writeFormEnd();
    Html.writeFooter();

    WebServer.send(200, ContentTypeHtml, HttpResponse.c_str());
}


void handleHttpBluetoothFormPost()
{
    Tracer tracer(F(__func__));

    int n = 0;
    for (int i = 0; i < WebServer.args(); i++)
    {
        if (WebServer.argName(i) == "uuid")
        {
            if (n == MAX_BT_DEVICES) continue;

            String uuidStr = WebServer.arg(i);
            TRACE("UUID: '%s'\n", uuidStr.c_str());

            UUID128 uuid = UUID128(uuidStr);
            memcpy(PersistentData.registeredBeacons[n++], uuid.data, sizeof(uuid128_t));
        }
    }
    
    PersistentData.registeredBeaconCount = n;
    PersistentData.writeToEEPROM();

    Bluetooth.registerBeacons(PersistentData.registeredBeaconCount, PersistentData.registeredBeacons);

    handleHttpBluetoothRequest();
}


void handleHttpBluetoothJsonRequest()
{
    Tracer tracer(F(__func__));

    if (state != EVSEState::Authorize)
    {
        Bluetooth.startDiscovery(2);
        while (Bluetooth.getState() == BluetoothState::Discovering)
        {
            delay(100);
        }
    }

    HttpResponse.clear();
    HttpResponse.print("[ ");
    bool first = true;
    for (BluetoothDeviceInfo& btDevice : Bluetooth.getDiscoveredDevices())
    {
        if (first)
            first = false;
        else
            HttpResponse.print(", ");

        HttpResponse.printf(
            F("{ \"rssi\": %d, \"bda\": \"%s\", \"name\": \"%s\", \"uuid\": \"%s\", \"manufacturer\": \"%s\", \"isRegistered\": %s }"),
            btDevice.rssi,
            btDevice.getAddress(),
            btDevice.name,
            (btDevice.uuid == nullptr) ? "(N/A)" : btDevice.uuid->toString().c_str(),
            btDevice.getManufacturerName(),
            btDevice.isRegistered ? "true" : "false"
            );
    }
    HttpResponse.println(" ]");

    WebServer.send(200, ContentTypeJson, HttpResponse.c_str());
}


void handleHttpChargeLogRequest()
{
    Tracer tracer(F(__func__));

    int currentPage = WebServer.hasArg("page") ? WebServer.arg("page").toInt() : 0;
    int totalPages = ((ChargeLog.count() - 1) / CHARGE_LOG_PAGE_SIZE) + 1;

    Html.writeHeader("Charge log", Nav);
    Html.writePager(totalPages, currentPage);
    Html.writeTableStart();

    Html.writeRowStart();
    Html.writeHeaderCell("Time");
    Html.writeHeaderCell("I<sub>limit</sub> (A)");
    Html.writeHeaderCell("I<sub>output</sub> (A)");
    Html.writeHeaderCell("T (°C)");
    Html.writeRowEnd();

    ChargeLogEntry* logEntryPtr = ChargeLog.getFirstEntry();
    for (int i = 0; i < (currentPage * CHARGE_LOG_PAGE_SIZE) && logEntryPtr != nullptr; i++)
    {
        logEntryPtr = ChargeLog.getNextEntry();
    }
    for (int i = 0; i < CHARGE_LOG_PAGE_SIZE && logEntryPtr != nullptr; i++)
    {
        Html.writeRowStart();
        Html.writeCell(formatTime("%d %b %H:%M", logEntryPtr->time));
        Html.writeCell(logEntryPtr->currentLimit);
        Html.writeCell(logEntryPtr->outputCurrent);
        Html.writeCell(logEntryPtr->temperature);
        Html.writeRowEnd();

        logEntryPtr = ChargeLog.getNextEntry();
    }

    Html.writeTableEnd();
    Html.writeFooter();

    WebServer.send(200, ContentTypeHtml, HttpResponse.c_str());
}


void handleHttpEventLogRequest()
{
    Tracer tracer(F(__func__));

    if (WiFiSM.shouldPerformAction("clear"))
    {
        EventLog.clear();
        WiFiSM.logEvent("Event log cleared.");
    }

    Html.writeHeader("Event log", Nav);

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


void handleHttpSyncFTPRequest()
{
    Tracer tracer("handleHttpSyncFTPRequest");

    Html.writeHeader("FTP Sync", Nav);

    HttpResponse.println("<pre>");
    bool success = trySyncFTP(&HttpResponse); 
    HttpResponse.println("</pre>");

    if (success)
    {
        Html.writeParagraph("Success!");
        ftpSyncTime = 0; // Cancel scheduled sync (if any)
    }
    else
        Html.writeParagraph("Failed: %s", FTPClient.getLastError());

    Html.writeHeading("CSV header", 2);
    HttpResponse.print("<pre>");
    HttpResponse.println("Time;Current Limit;Output Current;Temperature");
    HttpResponse.println("Start;Hours;Temperature;P (kW);E (kWh)");
    HttpResponse.println("</pre>");

    Html.writeFooter();

    WebServer.send(200, ContentTypeHtml, HttpResponse.c_str());
}


void handleHttpSmartMeterRequest()
{
    Tracer tracer(F(__func__));

    Html.writeHeader("Smart Meter", Nav);

    if (SmartMeter.isInitialized)
    {
        int dsmrResult = SmartMeter.requestData();
        if (dsmrResult == HTTP_CODE_OK)
        {
            std::vector<PhaseData> electricity = SmartMeter.getElectricity();

            Html.writeTableStart();
            Html.writeRowStart();
            Html.writeHeaderCell("Phase");
            Html.writeHeaderCell("Voltage");
            Html.writeHeaderCell("Current");
            Html.writeHeaderCell("P<sub>delivered</sub>");
            Html.writeHeaderCell("P<sub>returned</sub>");
            Html.writeRowEnd();
            for (PhaseData& phaseData : electricity)
            {
                Html.writeRowStart();
                Html.writeCell(phaseData.Name);
                Html.writeCell(phaseData.U, F("%0.1f V"));
                Html.writeCell(phaseData.I, F("%0.0f A"));
                Html.writeCell(phaseData.Pdelivered, F("%0.0f W"));
                Html.writeCell(phaseData.Preturned, F("%0.0f W"));
                Html.writeRowEnd();
            }
            Html.writeTableEnd();

            TRACE("DSMR phase: %d\n", PersistentData.dsmrPhase);

            PhaseData& monitoredPhaseData = electricity[PersistentData.dsmrPhase - 1];
            Html.writeParagraph(
                "Phase '%s' current: %0.1f A",
                monitoredPhaseData.Name.c_str(),
                monitoredPhaseData.Pdelivered / CHARGE_VOLTAGE);
        }
        else
            Html.writeParagraph(
                "%s returned %d: %s",
                PersistentData.dsmrMonitor,
                dsmrResult,
                SmartMeter.getLastError().c_str());
    }
    else
        Html.writeParagraph("Smart Meter is not enabled.");

    Html.writeParagraph(
        "Configured current limit: %d A",
        static_cast<int>(PersistentData.currentLimit));

    Html.writeParagraph(
        "Temperature: %0.1f °C => derated current limit: %0.1f A",
        temperature,
        getDeratedCurrentLimit());

    Html.writeParagraph(
        "Effective current limit: %0.1f A",
         determineCurrentLimit());

    Html.writeFooter();

    WebServer.send(200, ContentTypeHtml, HttpResponse.c_str());
}


void handleHttpCurrentRequest()
{
    Tracer tracer(F(__func__));

    bool raw = WebServer.hasArg("raw");

    if (state != EVSEState::Charging)
        OutputCurrentSensor.measure(10);

    HttpResponse.clear();
    OutputCurrentSensor.writeSampleCsv(HttpResponse, raw);

    WebServer.send(200, ContentTypeText, HttpResponse.c_str());
}


void handleHttpCalibrateRequest()
{
    Tracer tracer(F(__func__));

    bool savePersistentData = false;

    if (WiFiSM.shouldPerformAction(CAL_CURRENT_ZERO))
    {
        PersistentData.currentZero = OutputCurrentSensor.calibrateZero();
        savePersistentData = true;
    }

    if (WebServer.hasArg(CAL_CURRENT))
    {
        float actualCurrentRMS = WebServer.arg(CAL_CURRENT).toFloat();
        PersistentData.currentScale = OutputCurrentSensor.calibrateScale(actualCurrentRMS);
        savePersistentData = true;
    }

    if (WebServer.hasArg(CAL_TEMP_OFFSET))
    {
        PersistentData.tempSensorOffset = WebServer.arg(CAL_TEMP_OFFSET).toFloat();
        savePersistentData = true;
    }

    if (savePersistentData)
    {
        PersistentData.validate();
        PersistentData.writeToEEPROM();
    }

    if (state != EVSEState::Charging)
        OutputCurrentSensor.measure(50); // Measure 50 periods (1 s) for accuracy

    float outputCurrentRMS = OutputCurrentSensor.getRMS();
    float outputCurrentPeak = OutputCurrentSensor.getPeak();
    float outputCurrentDC = OutputCurrentSensor.getDC();
    float cpVoltage = ControlPilot.getVoltage();
    float cpDutyCycle = ControlPilot.getDutyCycle();
    float tMeasured = TempSensors.getTempC(PersistentData.tempSensorAddress);

    Html.writeHeader("Calibrate", Nav);

    Html.writeHeading("Control Pilot", 2);
    Html.writeTableStart();
    Html.writeRow("Measured", F("%0.2f V"), cpVoltage);
    Html.writeRow("Duty Cycle", F("%0.0f %%"), cpDutyCycle * 100);
    Html.writeTableEnd();

    Html.writeHeading("Output current", 2);
    Html.writeFormStart("/calibrate", "grid");
    HttpResponse.printf(F("<label>Samples</label><div>%d</div>\r\n"), OutputCurrentSensor.getSampleCount());
    HttpResponse.printf(F("<label>Measured (DC)</labeL><div>%0.1f mA "), outputCurrentDC * 1000);
    Html.writeActionLink(CAL_CURRENT_ZERO, "[Calibrate zero]", currentTime);
    Html.writeDivEnd();
    HttpResponse.printf(F("<label>Measured (Peak)</labeL><div>%0.2f A</div>\r\n"), outputCurrentPeak);
    HttpResponse.printf(F("<label>Measured (RMS)</labeL><div>%0.2f A</div>\r\n"), outputCurrentRMS);
    Html.writeNumberBox(CAL_CURRENT, "Actual (RMS)", outputCurrentRMS, 0, 20, 2);
    Html.writeSubmitButton("Calibrate");
    Html.writeFormEnd();

    Html.writeHeading("Temperature sensor", 2);
    Html.writeFormStart("/calibrate", "grid");
    HttpResponse.printf(F("<label>Measured</label><div>%0.2f °C</div>\r\n"), tMeasured);
    Html.writeTextBox(CAL_TEMP_OFFSET, "Offset", String(PersistentData.tempSensorOffset), 5);
    HttpResponse.printf(F("<label>Effective</label><div>%0.2f °C</div>\r\n"), tMeasured + PersistentData.tempSensorOffset);
    Html.writeSubmitButton("Calibrate");
    Html.writeFormEnd();

    Html.writeFooter();

    WebServer.send(200, ContentTypeHtml, HttpResponse.c_str());
}


void handleHttpConfigFormRequest()
{
    Tracer tracer(F(__func__));

    Html.writeHeader("Settings", Nav);

    Html.writeFormStart("/config", "grid");
    PersistentData.writeHtmlForm(Html);
    Html.writeSubmitButton("Save");
    Html.writeFormEnd();

    if (WiFiSM.shouldPerformAction("reset"))
        WiFiSM.reset();
    else
        Html.writeActionLink("reset", "Reset ESP", currentTime, ButtonClass);

    Html.writeFooter();

    WebServer.send(200, ContentTypeHtml, HttpResponse.c_str());
}


void handleHttpConfigFormPost()
{
    Tracer tracer(F(__func__));

    PersistentData.parseHtmlFormData([](const String& id) -> String { return WebServer.arg(id); });
    PersistentData.validate();
    PersistentData.writeToEEPROM();

    handleHttpConfigFormRequest();
}
