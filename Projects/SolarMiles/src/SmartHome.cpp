#include <Tracer.h>
#include <TimeUtils.h>
#include "SmartHome.h"

constexpr size_t NUM_SMARTTHINGS_CAPABILITIES = 4;

const char* _smartThingsCapabilities[] = 
{
    "switch",
    "powerMeter",
    "energyMeter",
    "powerConsumptionReport"
};

const char* _smartHomeStateLabels[] =
{ 
    "Uninitialized",
    "Initialized",
    "Connecting Fritzbox",
    "Discovering Fritz Devices",
    "Discovering SmartThings",
    "Ready"
};

const char* _smartDeviceStateLabels[] = { "Unknown", "Off", "On" };


const char* SmartHomeClass::getStateLabel()
{
    return _smartHomeStateLabels[(int)_state];
}


void SmartHomeClass::setState(SmartHomeState newState)
{
    _state = newState;
    _logger.logEvent("SmartHome: %s", getStateLabel());
}


bool SmartHomeClass::useFritzbox(const char* host, const char* user, const char* password)
{
    Tracer tracer("SmartHomeClass::useFritzbox", host);

    _fritzboxPtr = new TR064(49000, host, user, password, 2048);
    _fritzboxPtr->debug_level = TR064::LoggingLevels::DEBUG_INFO;
    return true;
}


bool SmartHomeClass::useSmartThings(const char* pat)
{
    Tracer tracer("SmartHomeClass::useSmartThings", pat);

    _smartThingsPtr = new SmartThingsClient(pat, _logger);
    return true;
}


bool SmartHomeClass::begin(float powerThreshold, uint32_t powerOffDelay, uint32_t pollInterval)
{
    Tracer tracer("SmartHomeClass::begin");

    _powerThreshold = powerThreshold;
    _powerOffDelay = powerOffDelay;
    _pollInterval = pollInterval;
    _currentDeviceIndex = 0;
    _nextActionMillis = 0;

    BaseType_t res = xTaskCreatePinnedToCore(
        run,
        "SmartHome",
        8192, // Stack Size (words)
        this,
        3, // Priority
        &_taskHandle,
        PRO_CPU_NUM); // Run on Core #0

    if (res != pdPASS)
    {
        _logger.logEvent("SmartHome: xTaskCreate returned %d\n", res);
        return false;
    }

    if (_fritzboxPtr != nullptr)
    {
        setState(SmartHomeState::ConnectingFritzbox);
        return true;
    }

    if (_smartThingsPtr != nullptr)
    {
        setState(SmartHomeState::DiscoveringSmartThings);
        return true;
    }

    return false;
}


bool SmartHomeClass::startDiscovery()
{
    Tracer tracer("SmartHomeClass::startDiscovery");

    if (_state != SmartHomeState::Ready)
        return false;

    if (_fritzboxPtr != nullptr)
        setState(SmartHomeState::DiscoveringFritzDevices);
    else if (_smartThingsPtr != nullptr)
        setState(SmartHomeState::DiscoveringSmartThings);
    else
        return false;
 
    _currentDeviceIndex =  0;
    devices.clear();
    energyLog.clear();
    return true;
}


void SmartHomeClass::writeHtml(HtmlWriter& html)
{
    html.writeDivStart("flex-container");

    html.writeSectionStart("Status");
    html.writeTableStart();
    html.writeRow("State", "%s", getStateLabel());
    if (_fritzboxPtr != nullptr)
        html.writeRow("Fritzbox", "%d ms", _fritzboxPtr->responseTimeMs());
    if (_smartThingsPtr != nullptr)
        html.writeRow("SmartThings", "%d ms", _smartThingsPtr->responseTimeMs());
    html.writeRow("Free Heap", "%0.1f kB", float(ESP.getMaxAllocHeap()) / 1024);

    html.writeTableEnd();
    html.writeSectionEnd();

    html.writeSectionStart("Devices");
    html.writeTableStart();
    html.writeRowStart();
    html.writeHeaderCell("Name");
    html.writeHeaderCell("State");
    html.writeHeaderCell("Switch");
    html.writeHeaderCell("P (W)");
    html.writeHeaderCell("E (kWh)");
    html.writeHeaderCell("Info");
    html.writeHeaderCell("Last on");
    html.writeHeaderCell("Duration");
    html.writeHeaderCell("ΔE (Wh)");
    html.writeRowEnd();
    int i = 0;
    for (SmartDevice* smartDevicePtr : devices)
    {
        String name = (i++ ==_currentDeviceIndex) ? "&bull; " : ""; 
        name += smartDevicePtr->name;

        html.writeRowStart();
        html.writeCell(name);
        html.writeCell(smartDevicePtr->getStateLabel());
        html.writeCell(smartDevicePtr->getSwitchStateLabel());
        html.writeCell(smartDevicePtr->power, F("%0.2f"));
        html.writeCell(smartDevicePtr->energy / 1000, F("%0.3f"));
        html.writeCell(smartDevicePtr->info);
        html.writeCell(formatTime("%a %H:%M", smartDevicePtr->energyLogEntry.start));
        html.writeCell(formatTimeSpan(smartDevicePtr->energyLogEntry.getDuration()));
        html.writeCell(smartDevicePtr->energyLogEntry.energyDelta, F("%0.0f"));
        html.writeRowEnd();
    }
    html.writeTableEnd();
    html.writeSectionEnd();

    html.writeSectionStart("Energy log");
    html.writeTableStart();
    html.writeRowStart();
    html.writeHeaderCell("Device");
    html.writeHeaderCell("Start");
    html.writeHeaderCell("Duration");
    html.writeHeaderCell("P<sub>max</sub> (W)");
    html.writeHeaderCell("P<sub>avg</sub> (W)");
    html.writeHeaderCell("E (Wh)");
    html.writeRowEnd();
    SmartDeviceEnergyLogEntry* logEntryPtr = energyLog.getFirstEntry();
    while (logEntryPtr != nullptr)
    {
        float avgPower = logEntryPtr->energyDelta * SECONDS_PER_HOUR / logEntryPtr->getDuration();

        html.writeRowStart();
        html.writeCell(logEntryPtr->devicePtr->name);
        html.writeCell(formatTime("%a %H:%M", logEntryPtr->start));
        html.writeCell(formatTimeSpan(logEntryPtr->getDuration()));
        html.writeCell(logEntryPtr->maxPower, F("%0.0f"));
        html.writeCell(avgPower, F("%0.0f"));
        html.writeCell(logEntryPtr->energyDelta, F("%0.0f"));
        html.writeRowEnd();

        logEntryPtr = energyLog.getNextEntry();
    }
    html.writeTableEnd();
    html.writeSectionEnd();

    html.writeDivEnd();
    html.writeFooter();
}


void SmartHomeClass::writeEnergyLogCsv(Print& output, bool onlyEntriesToSync)
{
    SmartDeviceEnergyLogEntry* logEntryPtr = onlyEntriesToSync
        ? energyLog.getEntryFromEnd(logEntriesToSync)
        : energyLog.getFirstEntry();
        
    while (logEntryPtr != nullptr)
    {
        logEntryPtr->writeCsv(output);
        logEntryPtr = energyLog.getNextEntry();
    }
}


void SmartHomeClass::run(void* taskParam)
{
    SmartHomeClass* instancePtr = static_cast<SmartHomeClass*>(taskParam); 
    while (true)
    {
        instancePtr->runStateMachine();
        delay(100);
    }
}

void SmartHomeClass::runStateMachine()
{
    uint32_t currentMillis = millis();
    if (currentMillis < _nextActionMillis) return;

    switch (_state)
    {
        case SmartHomeState::ConnectingFritzbox:
            _isAwaiting = true;
            _fritzboxPtr->init();
            _isAwaiting = false;
            if (_fritzboxPtr->state() == TR064_SERVICES_LOADED)
                setState(SmartHomeState::DiscoveringFritzDevices);
            else
            {
                _logger.logEvent("SmartHome: TR-064 connection failed");
                _nextActionMillis = currentMillis + SH_RETRY_DELAY_MS;
            }
            break;

        case SmartHomeState::DiscoveringFritzDevices:
            if (discoverFritzSmartPlug(devices.size()))
            {
                if (_smartThingsPtr == nullptr)
                    setState(SmartHomeState::Ready);
                else
                    setState(SmartHomeState::DiscoveringSmartThings);
            }
            break;

        case SmartHomeState::DiscoveringSmartThings:
            if (discoverSmartThings())
                setState(SmartHomeState::Ready);
            else
                _nextActionMillis = currentMillis + SH_RETRY_DELAY_MS;    
            break;

        case SmartHomeState::Ready:
            if (devices.size() > 0)
            {
                _nextActionMillis = currentMillis + _pollInterval * 1000;
                if (WiFi.status() == WL_CONNECTED)
                    updateDevice();
            }
            break;

        default:
            // Nothing to do
            break;        
    }
}


bool SmartHomeClass::discoverFritzSmartPlug(int index)
{
    TRACE("Discover FritzSmartPlug #%d...\n", index);

    _isAwaiting = true;
    FritzSmartPlug* fritzSmartPlugPtr = FritzSmartPlug::discover(index, _fritzboxPtr, _logger);
    _isAwaiting = false;
    if (fritzSmartPlugPtr != nullptr)
    {
        fritzSmartPlugPtr->powerThreshold = _powerThreshold;
        fritzSmartPlugPtr->powerOffDelay = _powerOffDelay; 
        devices.push_back(fritzSmartPlugPtr);
        return false;
    }

    if (_fritzboxPtr->errorCode() == TR064::TR064_CODE_ARRAYINDEXINVALID)
        return true; // Discoved all FritzSmartPlug devices

    _logger.logEvent(
        "FritzSmartPlug discovery error %d '%s'",
        _fritzboxPtr->errorCode(),
        _fritzboxPtr->errorDescription());

    _nextActionMillis = millis() + SH_RETRY_DELAY_MS;
    return false;
}


uint32_t getCapabilityFlags(JsonArray& jsonCapabilities)
{
    uint32_t result  = 0;

    for (JsonVariant jsonCapability : jsonCapabilities)
    {
        uint32_t capabilityFlag = 1;
        for (int i = 0; i < NUM_SMARTTHINGS_CAPABILITIES; i++)
        {
            String capabilityId = jsonCapability["id"];
            if (capabilityId == _smartThingsCapabilities[i])
            {
                result |= capabilityFlag;
                break;
            }
            capabilityFlag <<= 1;
        }
    }

    return result;
}


bool SmartHomeClass::discoverSmartThings()
{
    Tracer tracer("SmartHomeClass::discoverSmartThings");

    _isAwaiting = true;
    bool success = _smartThingsPtr->requestDevices();
    _isAwaiting = false;  
    if (!success) return false;

    JsonArray jsonDevices = _smartThingsPtr->jsonDoc["items"];
    TRACE("%d devices found\n", jsonDevices.size());

    for (JsonVariant jsonDevice : jsonDevices)
    {
        String deviceId = jsonDevice["deviceId"];
        String label = jsonDevice["label"];
        //TRACE("Device '%s'\n", label.c_str());

        JsonArray jsonComponents = jsonDevice["components"];
        for (JsonVariant jsonComponent : jsonComponents)
        {
            String componentId = jsonComponent["id"];
            //TRACE("\tComponent '%s'\n", componentId.c_str());
            if (componentId != "main") continue;

            JsonArray jsonCapabilities = jsonComponent["capabilities"];
            uint32_t capabilityFlags = getCapabilityFlags(jsonCapabilities);
            TRACE("Device '%s' Component 'main' Capabilities: %x\n", label.c_str(), capabilityFlags);

            if ((capabilityFlags & 7) == 7 || (capabilityFlags & 9) == 9)
            {
                SmartThingsDevice* smartThingsDevicePtr = new SmartThingsDevice(deviceId, label, _smartThingsPtr, _logger);
                if (capabilityFlags & 2) // powerMeter
                    smartThingsDevicePtr->powerThreshold = _powerThreshold;
                else
                    smartThingsDevicePtr->powerThreshold = 0;
                smartThingsDevicePtr->powerOffDelay = _powerOffDelay; 
                devices.push_back(smartThingsDevicePtr);
            }
        }
    }

    _smartThingsPtr->cleanup();
    return true;    
}


bool SmartHomeClass::updateDevice()
{
    time_t currentTime = time(nullptr);

    TRACE("Updating device #%d...\n", _currentDeviceIndex);

    SmartDevice* smartDevicePtr = devices[_currentDeviceIndex];
    SmartDeviceState deviceStateBefore = smartDevicePtr->state;

    _isAwaiting = true;
    bool success = smartDevicePtr->update(currentTime);
    _isAwaiting = false;

    if (++_currentDeviceIndex >= devices.size())
        _currentDeviceIndex = 0;

    if (!success) return false;

    if (deviceStateBefore == SmartDeviceState::On && smartDevicePtr->state == SmartDeviceState::Off)
    {
        // Device switched off; update energy log
        if (smartDevicePtr->energyLogEntry.energyDelta >= 1.0F)
        {
            energyLog.add(&smartDevicePtr->energyLogEntry);
            logEntriesToSync = std::min(logEntriesToSync + 1, energyLog.size());
        }
    }
    return true;
}


void SmartDeviceEnergyLogEntry::writeCsv(Print& output)
{
    output.printf("%s;", formatTime("%F %H:%M", start));
    output.printf("%s;", devicePtr->name.c_str());
    output.printf("%0.1f;", float(getDuration()) / SECONDS_PER_HOUR);
    output.printf("%0.0f;", maxPower);
    output.printf("%0.0f", energyDelta);
    output.println();
}


const char* SmartDevice::getStateLabel()
{
    return _smartDeviceStateLabels[(int)state];
}


const char* SmartDevice::getSwitchStateLabel()
{
    return _smartDeviceStateLabels[(int)switchState];
}


bool SmartDevice::update(time_t currentTime)
{
    if (state == SmartDeviceState::Unknown)
        state = SmartDeviceState::Off;

    if (switchState == SmartDeviceState::Off)
    {
        // Switch is off, but we can delay switching the device off so there is some time to update the energy
        // before we commit it to the energy log.
        if ((powerThreshold > 0) || (currentTime > energyLogEntry.end + powerOffDelay))
        {
            if (state == SmartDeviceState::On)
                _logger.logEvent("Smart Device '%s' off", name.c_str());
            state = SmartDeviceState::Off;
        }
        else
            energyLogEntry.energyDelta = energy - energyLogEntry.energyStart;
    }
    else
    {
        // Switch is on, but that doesn't mean the device is on; check the power consumption.
        if (power >= powerThreshold)
        {
            if (state == SmartDeviceState::Off)
            {
                _logger.logEvent("Smart Device '%s' on", name.c_str());
                state = SmartDeviceState::On;
                energyLogEntry.reset(currentTime, energy);
            }
            energyLogEntry.update(currentTime, energy, power);
        }
        else if (currentTime > energyLogEntry.end + powerOffDelay)
        {
            if (state == SmartDeviceState::On)
                _logger.logEvent("Smart Device '%s' idle", name.c_str());
            state = SmartDeviceState::Off;
        }
    }

    return true;
}


FritzSmartPlug* FritzSmartPlug::discover(int index, TR064* fritzboxPtr, ILogger& logger)
{
    Tracer tracer("FritzSmartPlug::discover");

    String params[][2] = {{"NewIndex", String(index)}};
    String fields[][2] = {{"NewAIN", ""}, {"NewDeviceName", ""}};

    if (fritzboxPtr->action("X_AVM-DE_Homeauto:1", "GetGenericDeviceInfos", params, 1, fields, 2))
        return new FritzSmartPlug(fields[0][1], fields[1][1], fritzboxPtr, logger);
    else
        return nullptr;
}


bool FritzSmartPlug::update(time_t currentTime)
{
    Tracer tracer("FritzSmartPlug::update", id.c_str());

    String params[][2] = {{"NewAIN", id}};
    String fields[][2] = 
    {
        {"NewPresent", ""},
        {"NewSwitchIsValid", ""},
        {"NewSwitchState", ""},
        {"NewMultimeterIsValid", ""},
        {"NewMultimeterPower", ""},
        {"NewMultimeterEnergy", ""},
        {"NewTemperatureCelsius", ""}
    };

    if (!_fritzboxPtr->action("X_AVM-DE_Homeauto:1", "GetSpecificDeviceInfos", params, 1, fields, 7))
    {
        int errorCode = _fritzboxPtr->errorCode(); 
        if (errorCode != _lastErrorCode)
            _logger.logEvent("FritzSmartPlug: %s", _fritzboxPtr->errorDescription());
        _lastErrorCode = errorCode; 
        return false;
    }
    _lastErrorCode = 0;

    if (fields[0][1] != "CONNECTED")
    {
        state = SmartDeviceState::Unknown;
        return false;
    }

    if (fields[1][1] != "VALID")
    {
        _logger.logEvent("FritzSmartPlug: SwitchIsValid='%s'", fields[1][1].c_str());
        return false;
    }

    if (fields[3][1] != "VALID")
    {
        _logger.logEvent("FritzSmartPlug: MultimeterIsValid='%s'", fields[3][1].c_str());
        return false;
    }

    if (fields[2][1] == "OFF")
        switchState = SmartDeviceState::Off;
    else if (fields[2][1] == "ON")
        switchState = SmartDeviceState::On;
    else
    {
        _logger.logEvent("FritzSmartPlug: SwitchState='%s'", fields[2][1].c_str());
        return false;
    }

    power = fields[4][1].toFloat() / 100;
    energy = fields[5][1].toFloat();
    float temperature = fields[6][1].toFloat() / 10;
    info = String(temperature, 1);
    info += " °C";

    return SmartDevice::update(currentTime);
}


bool SmartThingsDevice::update(time_t currentTime)
{
    Tracer tracer("SmartThingsDevice::update", id.c_str());

    if (!_smartThingsPtr->requestDeviceStatus(id))
        return false;

    String switchValue = _smartThingsPtr->jsonDoc["switch"]["switch"]["value"];
    TRACE("switch: '%s'\n", switchValue.c_str());
    switchState = (switchValue == "on") ? SmartDeviceState::On : SmartDeviceState::Off;

    JsonVariant jsonPowerConsumption = _smartThingsPtr->jsonDoc["powerConsumptionReport"]["powerConsumption"];

    JsonVariant jsonPowerMeter = _smartThingsPtr->jsonDoc["powerMeter"];
    if (!jsonPowerMeter.isNull())
        power = jsonPowerMeter["power"]["value"];
    else if (!jsonPowerConsumption.isNull())
        power = jsonPowerConsumption["value"]["power"];
    else
        _logger.logEvent("SmartThingsDevice: No power found");
    
    JsonVariant jsonEnergyMeter = _smartThingsPtr->jsonDoc["energyMeter"];
    if (!jsonEnergyMeter.isNull())
    {
        energy = jsonEnergyMeter["energy"]["value"];
        if (jsonEnergyMeter["energy"]["unit"].as<String>() == "kWh")
            energy *= 1000;
    }
    else if (!jsonPowerConsumption.isNull())
    {
        float energyValue = jsonPowerConsumption["value"]["energy"];
        if (energyValue > 0)
            energy = energyValue;
        else
        {
            String timestamp = jsonPowerConsumption["timestamp"];
            TRACE("Timestamp: '%s'\n", timestamp.c_str());
            if (timestamp != _powerConsumptionTimestamp)
            {
                _powerConsumptionTimestamp = timestamp;
                float deltaEnergy = jsonPowerConsumption["value"]["deltaEnergy"];
                TRACE("deltaEnergy: %0.2f\n", deltaEnergy);
                energy += deltaEnergy; 
            }
        } 
    }
    else
        _logger.logEvent("SmartThingsDevice: No energy found");
    
    JsonVariant jsonWashingCourse = _smartThingsPtr->jsonDoc["samsungce.dishwasherWashingCourse"]["washingCourse"];
    if (!jsonWashingCourse.isNull())
        info = jsonWashingCourse["value"].as<String>();

    JsonVariant jsonRemainingTime = _smartThingsPtr->jsonDoc["samsungce.dishwasherOperation"]["remainingTimeStr"];
    if (!jsonRemainingTime.isNull())
    {
        info += " ";
        info += jsonRemainingTime["value"].as<String>();
    }

    _smartThingsPtr->cleanup();

    return SmartDevice::update(currentTime);
}
