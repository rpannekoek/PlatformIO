#include "WiFiStateMachine.h"
#include <Arduino.h>
#include <ArduinoOTA.h>
#include <ESPWiFi.h>
#include <ESPFileSystem.h>
#include <ESPCoreDump.h>
#include <Tracer.h>
#include <StringBuilder.h>

#ifdef ESP32
    #include <rom/rtc.h>
    #include <esp_wifi.h>
    #include <esp_task_wdt.h>
    constexpr int TASK_WDT_TIMEOUT = 30;

    SemaphoreHandle_t WiFiStateMachine::_logMutex = xSemaphoreCreateMutex();
#else
    #define U_SPIFFS U_FS
#endif

constexpr uint32_t CONNECT_TIMEOUT_MS = 10000;
constexpr uint32_t MIN_RETRY_INTERVAL_MS = 5000;
constexpr uint32_t MAX_RETRY_INTERVAL_MS = 300000;

bool WiFiStateMachine::_staDisconnected = false;
StringBuilder _coreDumpBuilder(256);

// Constructor
WiFiStateMachine::WiFiStateMachine(LED& led, WiFiNTP& timeServer, ESPWebServer& webServer, Log<const char>& eventLog)
    : _led(led), _timeServer(timeServer), _webServer(webServer)
{
    _eventLogPtr = &eventLog;
    _eventStringLogPtr = nullptr;
    memset(_handlers, 0, sizeof(_handlers));
}

// Constructor
WiFiStateMachine::WiFiStateMachine(LED& led, WiFiNTP& timeServer, ESPWebServer& webServer, StringLog& eventLog)
    : _led(led), _timeServer(timeServer), _webServer(webServer)
{
    _eventStringLogPtr = &eventLog;
    _eventLogPtr = nullptr;
    memset(_handlers, 0, sizeof(_handlers));
}


void WiFiStateMachine::on(WiFiInitState state, void (*handler)(void))
{
    _handlers[static_cast<int>(state)] = handler;
}


void WiFiStateMachine::registerStaticFiles(PGM_P* files, size_t count)
{
    if (!SPIFFS.begin())
    {
        logEvent(F("Starting SPIFFS failed"));
        return;
    }

    for (int i = 0; i < count; i++)
    {
        String path = F("/");
        path += FPSTR(files[i]);
        _webServer.serveStatic(path.c_str(), SPIFFS, path.c_str(), "max-age=86400, public");
    }
}


void WiFiStateMachine::begin(String ssid, String password, String hostName, uint32_t reconnectInterval)
{
    Tracer tracer(F("WiFiStateMachine::begin"), hostName.c_str());

    _reconnectInterval = reconnectInterval * 1000;
    _ssid = ssid;
    _password = password;
    _hostName = hostName;
    _retryInterval = MIN_RETRY_INTERVAL_MS;
    _isTimeServerAvailable = false;
    _resetMillis = 0;

    logEvent(F("Booted from %s"), getResetReason().c_str());
    logEvent(F("CPU @ %d MHz"), ESP.getCpuFreqMHz());

#ifdef ESP32
    esp_core_dump_init();
#if (ESP_ARDUINO_VERSION_MAJOR == 2)
    esp_task_wdt_init(TASK_WDT_TIMEOUT, true);
#else
    esp_task_wdt_config_t wdtConfig = 
    {
        .timeout_ms = TASK_WDT_TIMEOUT * 1000,
        .idle_core_mask = 1,
        .trigger_panic = true
    };
    esp_task_wdt_reconfigure(&wdtConfig);
#endif
    enableLoopWDT();
#endif

    ArduinoOTA.onStart(
        [this]() 
        {
            TRACE(F("OTA start %d\n"), ArduinoOTA.getCommand());
            if (ArduinoOTA.getCommand() == U_SPIFFS)
                SPIFFS.end();
#ifdef ESP32
            disableLoopWDT();
#endif                
            setState(WiFiInitState::Updating, true);
        });

    ArduinoOTA.onEnd(
        [this]()
        {
            TRACE(F("OTA end %d\n"), ArduinoOTA.getCommand());
            setState(WiFiInitState::Initialized);
        });

    ArduinoOTA.onError(
        [this](ota_error_t error)
        {
            TRACE(F("OTA error %u\n"), error);
            setState(WiFiInitState::Initialized);
        });

#ifdef ESP8266
    _staDisconnectedEvent = WiFi.onStationModeDisconnected(&WiFiStateMachine::onStationDisconnected);
#else
    _staDisconnectedEvent = WiFi.onEvent(WiFiStateMachine::onStationDisconnected, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
#endif

    _webServer.on("/coredump", std::bind(&WiFiStateMachine::handleHttpCoreDump, this));
    _webServer.onNotFound(std::bind(&WiFiStateMachine::handleHttpNotFound, this));

    setState(WiFiInitState::Initializing);
}


void WiFiStateMachine::forceReconnect(const uint8_t* bssid)
{
    Tracer tracer(F("WiFiStateMachine::forceReconnect"));

#ifdef ESP8266
    if (!WiFi.reconnect())
        TRACE(F("WiFi.reconnect() failed.\n"));
#else
    // It seems ESP32 needs more force to make it forget the earlier AP
    if (!WiFi.STA.disconnect(true, 1000))
        logEvent("WiFi.STA.disconnect failed");
    if (!WiFi.STA.connect(_ssid.c_str(), _password.c_str(), 0, bssid))
        logEvent("WiFi.STA.connect failed");
#endif
}


time_t WiFiStateMachine::getCurrentTime()
{
    if (_isTimeServerAvailable)
        return _timeServer.getCurrentTime();
    else
        return millis() / 1000;
}


void WiFiStateMachine::traceDiag()
{
#ifdef DEBUG_ESP_PORT
    WiFi.printDiag(DEBUG_ESP_PORT);

#ifdef ESP32
    wifi_config_t wifiConfig;
    esp_err_t err = esp_wifi_get_config((wifi_interface_t)ESP_IF_WIFI_STA, &wifiConfig);
    if (err != ESP_OK)
    {
        TRACE("esp_wifi_get_config() returned %d\n", err);
        return;
    }

    uint8_t* bssid = wifiConfig.sta.bssid;
    TRACE(
        "bssid: %02X:%02X:%02X:%02X:%02X:%02X\n",
        bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5]);
    TRACE("btm_enabled: %d\n", wifiConfig.sta.btm_enabled);
    TRACE("scan_method: %d\n", wifiConfig.sta.scan_method);
    TRACE("sort_method: %d\n", wifiConfig.sta.sort_method);
    TRACE("threshold.rssi: %d\n", wifiConfig.sta.threshold.rssi);
    TRACE("failure_retry_cnt: %d\n", wifiConfig.sta.failure_retry_cnt);
#endif
#endif
}


void WiFiStateMachine::logEvent(String format, ...)
{
    char logMessage[64];

    va_list args;
    va_start(args, format);
    vsnprintf(logMessage, sizeof(logMessage), format.c_str(), args);
    va_end(args);

    logMessage[sizeof(logMessage) -1 ] = 0; // Ensure the string is always null-terminated

    logEvent(logMessage);
}


void WiFiStateMachine::logEvent(const char* msg)
{
#ifdef ESP32
    xSemaphoreTake(_logMutex, pdMS_TO_TICKS(100));
#endif
    TRACE("logEvent: %s\n", msg);

    size_t timestamp_size = 23; // strlen("2019-01-30 12:23:34 : ") + 1;
    char* event = new char[timestamp_size + strlen(msg)];

    if (_isTimeServerAvailable)
    {
        time_t currentTime = _timeServer.getCurrentTime();
        strftime(event, timestamp_size, "%F %H:%M:%S : ", localtime(&currentTime));
    }
    else
        snprintf(event, timestamp_size, "@ %lu ms : ", static_cast<uint32_t>(millis()));

    strcat(event, msg);

    if (_eventStringLogPtr == nullptr)
        _eventLogPtr->add(event);
    else
    {
        _eventStringLogPtr->add(event);
        delete[] event;
    }

#ifdef ESP32
    xSemaphoreGive(_logMutex);
#endif
}


void WiFiStateMachine::setState(WiFiInitState newState, bool callHandler)
{
    uint32_t prevStateChangeTime = _stateChangeTime;
    _stateChangeTime = millis();
    TRACE(
        F("WiFi state: %d -> %d @ +%u ms\n"),
        _state,
        newState,
        _stateChangeTime - prevStateChangeTime);

    _state = newState;
    if (callHandler)
    {
        int state = static_cast<int>(_state);
        if (_handlers[state] != nullptr)
            _handlers[state]();
    }
}


void WiFiStateMachine::initializeAP()
{
    TRACE(F("Starting WiFi network '%s' ...\n"), _hostName.c_str());

    WiFi.persistent(false);
    if (!WiFi.mode(WIFI_AP))
        TRACE(F("Unable to set WiFi mode\n"));

    if (!WiFi.softAP(_hostName.c_str()))
        TRACE(F("Unable to start Access Point\n"));

    _ipAddress = WiFi.softAPIP();
    logEvent(F("Started Access Point mode. IP address: %s"), getIPAddress().c_str());
}


void WiFiStateMachine::initializeSTA()
{
    TRACE(F("Connecting to WiFi network '%s' ...\n"), _ssid.c_str());
    WiFi.persistent(false);
    if (!WiFi.setAutoReconnect(_reconnectInterval == 0))
        TRACE(F("Unable to set auto reconnect\n"));
#ifdef ESP8266
    if (!WiFi.mode(WIFI_STA))
        TRACE(F("Unable to set WiFi mode\n"));
    if (!WiFi.disconnect())
        TRACE(F("WiFi disconnect failed\n"));
    if (!WiFi.hostname(_hostName))
        TRACE(F("Unable to set host name\n"));
#else
    if (!WiFi.mode(WIFI_OFF))
        TRACE(F("Unable to set WiFi mode (OFF)\n"));
    if (!WiFi.setHostname(_hostName.c_str()))
        TRACE(F("Unable to set host name ('%s')\n"), _hostName.c_str());
    if (!WiFi.mode(WIFI_STA))
        TRACE(F("Unable to set WiFi mode (STA)\n"));
    WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
    WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);
#endif
    ArduinoOTA.setHostname(_hostName.c_str());
    WiFi.begin(_ssid.c_str(), _password.c_str());
}


void WiFiStateMachine::run()
{
    uint32_t currentMillis = millis();
    uint32_t currentStateMillis = currentMillis - _stateChangeTime;
    wl_status_t wifiStatus = WiFi.status();
    String event;

    if ((_ledBlinkInterval != 0) && (currentMillis >= _ledBlinkMillis))
    {
        _ledBlinkMillis = currentMillis + _ledBlinkInterval;
        _led.toggle();
    }

    // First trigger custom handler (if any)
    void (*handler)(void) = _handlers[static_cast<int>(_state)]; 
    if (handler != nullptr) handler();

    switch (_state)
    {
        case WiFiInitState::Initializing:
            if (_ssid.length() == 0)
            {
                initializeAP();
                _isInAccessPointMode = true;
                setState(WiFiInitState::AwaitingConnection);
            }
            else
            {
                initializeSTA();
                _staDisconnected = false;
                _isInAccessPointMode = false;
                setState(WiFiInitState::Connecting);
            }
            TRACE(F("WiFi status: %d\n"), WiFi.status());
            break;

        case WiFiInitState::AwaitingConnection:
            blinkLED(1000);
            if (WiFi.softAPgetStationNum() > 0)
            {
                traceDiag();
                _webServer.begin();
                // Skip actual time server sync (no internet access), but still trigger TimeServerSynced event.
                setState(WiFiInitState::TimeServerSynced);
            }
            break;

        case WiFiInitState::Connecting:
            blinkLED(300);
            if (wifiStatus == WL_CONNECTED)
                setState(WiFiInitState::Connected);
            else if (wifiStatus == WL_CONNECT_FAILED)
                setState(WiFiInitState::ConnectFailed); 
            else if (currentStateMillis >= CONNECT_TIMEOUT_MS)
            {
                TRACE(F("Timeout connecting WiFi.\n"));
                setState(WiFiInitState::ConnectFailed);
            }
            break;

        case WiFiInitState::Reconnecting:
            if (wifiStatus == WL_CONNECTED)
            {
                traceDiag();
                logEvent(F("WiFi reconnected. Access Point %s\n"), WiFi.BSSIDstr().c_str());
                if (_scanAccessPointsTime > 0)
                    _scanAccessPointsTime = std::max(_scanAccessPointsTime, (time_t)(getCurrentTime() + _scanAccessPointsInterval));
                setState(WiFiInitState::Initialized);
            }
            else if (_staDisconnected || (wifiStatus == WL_NO_SSID_AVAIL) || (currentStateMillis >= CONNECT_TIMEOUT_MS))
            {
                TRACE(F("Reconnecting WiFi failed. Status: %d\n"), wifiStatus);
#ifdef ESP8266
                if (!WiFi.forceSleepBegin())
                    TRACE(F("forceSleepBegin() failed.\n"));
#endif
                setState(WiFiInitState::ConnectionLost);
            }
            else
            {
                // Still also trigger Initialized handler (for backwards compatibility)
                void (*initHandler)(void) = _handlers[static_cast<int>(WiFiInitState::Initialized)]; 
                if (initHandler != nullptr) initHandler();
            }
            break;

        case WiFiInitState::ConnectionLost:
            if (wifiStatus == WL_CONNECTED)
            {
                traceDiag();
                logEvent(F("WiFi reconnected. Access Point %s"), WiFi.BSSIDstr().c_str());
                _staDisconnected = false;
                setState(WiFiInitState::Initialized);
            }
            else if ((_reconnectInterval != 0) && (currentStateMillis >= _reconnectInterval))
            {
                TRACE(F("Attempting WiFi reconnect...\n"));
                _staDisconnected = false;
#ifdef ESP8266
                if (!WiFi.forceSleepWake())
                    TRACE(F("forceSleepWake() failed.\n"));
#else
                if (!WiFi.reconnect())
                    TRACE(F("reconnect() failed.\n"));
#endif
                TRACE(F("WiFi status: %d\n"), WiFi.status());
                setState(WiFiInitState::Reconnecting);
            }
            else
            {
                // Still also trigger Initialized handler (for backwards compatibility)
                void (*initHandler)(void) = _handlers[static_cast<int>(WiFiInitState::Initialized)]; 
                if (initHandler != nullptr) initHandler();
            }
            break;

        case WiFiInitState::SwitchingAP:
            blinkLED(300);
            if (_staDisconnected || currentStateMillis > CONNECT_TIMEOUT_MS)
            {
                _staDisconnected = false;
                setState(WiFiInitState::Reconnecting);
            }
            break;

        case WiFiInitState::ConnectFailed:
            if (currentStateMillis >= _retryInterval)
            {
                // Exponential backoff
                _retryInterval = std::min(_retryInterval * 2, MAX_RETRY_INTERVAL_MS);
                setState(WiFiInitState::Initializing);
            }
            break;

        case WiFiInitState::Connected:
            traceDiag();
            _staDisconnected = false;
            _ipAddress = WiFi.localIP();
            logEvent(F("WiFi connected. Access Point %s"), WiFi.BSSIDstr().c_str());
            ArduinoOTA.begin();
            _webServer.begin();
            setState(WiFiInitState::TimeServerInitializing);
            break;

        case WiFiInitState::TimeServerInitializing:
            blinkLED(500);
            _timeServer.beginGetServerTime(); // Ensure SNTP is initialized
            setState(WiFiInitState::TimeServerSyncing);
            break;

        case WiFiInitState::TimeServerSyncing:
            _initTime = _timeServer.endGetServerTime(); 
            if (_initTime != 0)
            {
                logEvent(F("Time synchronized using NTP server: %s"), _timeServer.NTPServer);
                _isTimeServerAvailable = true;
                blinkLED(0);
                setState(WiFiInitState::TimeServerSynced);
            }
            break;
        
        case WiFiInitState::TimeServerSynced:
            logEvent(F("WiFi initialized"));
            if (!_isInAccessPointMode && (_scanAccessPointsInterval > 0))
                _scanAccessPointsTime = getCurrentTime() + _scanAccessPointsInterval;
            setState(WiFiInitState::Initialized);
            break;

        case WiFiInitState::Initialized:
            blinkLED(0);
            if (!_isInAccessPointMode && (_staDisconnected || (wifiStatus != WL_CONNECTED)))
            {
                logEvent(F("WiFi connection lost"));
                TRACE(F("WiFi status: %d\n"), wifiStatus);
                if (_reconnectInterval != 0)
                {
#ifdef ESP8266
                    if (!WiFi.forceSleepBegin())
                        TRACE(F("forceSleepBegin() failed.\n"));
#endif
                }
                setState(WiFiInitState::ConnectionLost);
            }
            else if (_scanAccessPointsTime > 0)
                scanForBetterAccessPoint();
            
            break;

        default:
            // Nothing to do
            break;
    }

    // Automatic Modem sleep leverages delay() to reduce power
    if (_state > WiFiInitState::Connected)
    {
        _webServer.handleClient();
        ArduinoOTA.handle();
        delay(activeDelay);
    }
    else
        delay(inactiveDelay);

    if ((_resetMillis > 0) && (currentMillis >= _resetMillis))
    {
        TRACE(F("Resetting...\n"));
        ESP.restart();
        // From non-OS SDK Reference:
        // The ESP8266 will not restart immediately. Please do not call other functions after calling this API. 
        delay(1000); 
    }
}


void WiFiStateMachine::scanForBetterAccessPoint()
{

    time_t currentTime = getCurrentTime();
    if (currentTime >= _scanAccessPointsTime)
    {
        uint8_t currentChannel = WiFi.channel();
        TRACE(F("Scanning for better Access Point (SSID: '%s', channel %d)...\n"), _ssid.c_str(), currentChannel);
#ifdef ESP32
        int scanResult = WiFi.scanNetworks(
            true, // async
            false, // skip hidden SSID
            true, // passive
            300, // max_ms_per_chan
            currentChannel,
            _ssid.c_str()); 
#else
        int scanResult = WiFi.scanNetworks(
            true, // async
            false, // skip hidden SSID
            currentChannel,
            (uint8_t*)_ssid.c_str()); 
#endif
        if (scanResult != WIFI_SCAN_RUNNING)
            logEvent(F("WiFi scan failed"));
        _scanAccessPointsTime = currentTime + _scanAccessPointsInterval;
        return;
    }

    int8_t scannedAPs = WiFi.scanComplete();
    if (scannedAPs < 0) return; // Scan failed or still pending

    if (scannedAPs == 1)
        TRACE(F("Found only one Access Point.\n"));
    else
    {
        TRACE(F("Found %d Access Points:\n"), scannedAPs);

        int bestAP = 0;
        int8_t bestRSSI = -100;
        int8_t currentRSSI = 0;
        String bestBSSID;
        String currentBSSID = WiFi.BSSIDstr();

        for (int i = 0; i < scannedAPs; i++)
        {
            String bssid = WiFi.BSSIDstr(i);
            int8_t rssi = WiFi.RSSI(i);
            TRACE(F("BSSID: %s (RSSI: %d dBm)\n"), bssid.c_str(), rssi);
            if (rssi > bestRSSI)
            {
                bestAP = i;
                bestRSSI = rssi;
                bestBSSID = bssid;               
            }
            if (bssid == currentBSSID) currentRSSI = rssi;
        }

        if ((bestBSSID != currentBSSID && bestRSSI > (currentRSSI + _rssiThreshold)) || (_rssiThreshold == 0))
        {
            logEvent(F("Found better Access Point: %s (%d vs %d dBm)"), bestBSSID.c_str(), bestRSSI, currentRSSI);
            uint8_t bssid[6];
#ifdef ESP32            
            WiFi.BSSID(bestAP, bssid);
#endif
            _staDisconnected = false;
            forceReconnect(bssid);
            _scanAccessPointsTime = getCurrentTime() + _scanAccessPointsInterval + _switchAccessPointDelay;
            setState(WiFiInitState::SwitchingAP);
        }
        else
            TRACE(F("Sticking with current AP: %s\n"), currentBSSID.c_str());
    }

    WiFi.scanDelete();

    TRACE(F("Next scan in %d seconds.\n"), (int)(_scanAccessPointsTime - currentTime));
}


void WiFiStateMachine::reset()
{
    _resetMillis = millis() + 1000;
}


void WiFiStateMachine::blinkLED(uint32_t interval)
{
    if ((interval == 0) && (_ledBlinkInterval != 0))
        _led.setOn(false);
    _ledBlinkInterval = interval;
}


String WiFiStateMachine::getResetReason()
{
#ifdef ESP8266
    return ESP.getResetReason();
#else
    switch (esp_reset_reason())
    {
        case ESP_RST_POWERON: return "power on reset";
        case ESP_RST_EXT: return "external pin reset";
        case ESP_RST_SW: return "software reset";
        case ESP_RST_PANIC: return "exception/panic reset";
        case ESP_RST_INT_WDT: return "interrupt watchdog reset";
        case ESP_RST_TASK_WDT: return "task watchdog reset";
        case ESP_RST_WDT: return "watchdog reset";
        case ESP_RST_DEEPSLEEP: return "exiting deep sleep";
        case ESP_RST_BROWNOUT: return "brownout reset";
        case ESP_RST_SDIO: return "SDIO reset";
        default: return "unknown reset";
    }
#endif
}


bool WiFiStateMachine::shouldPerformAction(String name)
{
    if (!_webServer.hasArg(name))
        return false; // Action not requested

    time_t actionTime = _webServer.arg(name).toInt();

    if (actionTime <= _actionPerformedTime)
        return false; // Action already performed

    _actionPerformedTime = actionTime;
    return true;
}


#ifdef ESP8266
void WiFiStateMachine::onStationDisconnected(const WiFiEventStationModeDisconnected& evt)
{
    TRACE(F("STA disconnected. Reason: %d\n"), evt.reason);
    _staDisconnected = true;
}
#else
void WiFiStateMachine::onStationDisconnected(arduino_event_id_t event, arduino_event_info_t info)
{
    TRACE(F("STA disconnected. Reason: %d\n"), info.wifi_sta_disconnected.reason);
    _staDisconnected = true;
}
#endif


void WiFiStateMachine::handleHttpCoreDump()
{
    Tracer tracer("WiFiStateMachine::handleHttpCoreDump");

    _coreDumpBuilder.clear();
    writeCoreDump(_coreDumpBuilder);
    _webServer.send(200, "text/plain", _coreDumpBuilder.c_str());
}

void WiFiStateMachine::handleHttpNotFound()
{
    logEvent("Unexpected HTTP request: %s", _webServer.uri().c_str());
    _webServer.send(404, "text/plain", "Unexpected request.");
}
