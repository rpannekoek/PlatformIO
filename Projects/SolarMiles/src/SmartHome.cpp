#include <Tracer.h>
#include "SmartHome.h"

const char* _smartHomeStateLabels[] =
{ 
    "Uninitialized",
    "Initialized",
    "Connecting Fritzbox",
    "Discovering Devices",
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


uint32_t SmartHomeClass::getResponseTimeMs()
{
    return (_fritzboxPtr == nullptr) ? 0 : _fritzboxPtr->responseTimeMs();    
}


bool SmartHomeClass::begin(float powerThreshold, uint32_t powerOffDelay, uint32_t pollInterval)
{
    Tracer tracer("SmartHomeClass::begin");

    _powerThreshold = powerThreshold;
    _powerOffDelay = powerOffDelay;
    _pollInterval = pollInterval;
    _currentDeviceIndex = 0;

    BaseType_t res = xTaskCreate(
        run,
        "SmartHome",
        8192, // Stack Size (words)
        this,
        3, // Priority
        &_taskHandle);

    if (res != pdPASS)
    {
        _logger.logEvent("SmartHome: xTaskCreate returned %d\n", res);
        return false;
    }

    setState(SmartHomeState::Initialized);
    return true;
}


bool SmartHomeClass::useFritzbox(const char* host, const char* user, const char* password)
{
    Tracer tracer("SmartHomeClass::useFritzbox", host);

    if (_state != SmartHomeState::Initialized)
        return false;

    _fritzboxPtr = new TR064(49000, host, user, password);
    _fritzboxPtr->debug_level = TR064::LoggingLevels::DEBUG_INFO;

    setState(SmartHomeState::ConnectingFritzbox);
    return true;
}


bool SmartHomeClass::startDiscovery()
{
    Tracer tracer("SmartHomeClass::startDiscovery");

    if (_state != SmartHomeState::Ready || _fritzboxPtr == nullptr)
        return false;

    devices.clear();
    energyLog.clear();

    setState(SmartHomeState::DiscoveringDevices);
    return true;
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
    FritzSmartPlug* fritzSmartPlugPtr;
    int index;

    switch (_state)
    {
        case SmartHomeState::ConnectingFritzbox:
            _fritzboxPtr->init();
            if (_fritzboxPtr->state() == TR064_SERVICES_LOADED)
                setState(SmartHomeState::DiscoveringDevices);
            else
            {
                _logger.logEvent("SmartHome: TR-064 connection failed. Retry in %d s...", SH_RETRY_DELAY);
                errors++;
                delay(SH_RETRY_DELAY * 1000);
            }
            break;

        case SmartHomeState::DiscoveringDevices:
            index = devices.size();
            TRACE("Discover device #%d...\n", index);
            fritzSmartPlugPtr = FritzSmartPlug::discover(index, _fritzboxPtr, _logger);
            if (fritzSmartPlugPtr != nullptr)
            {
                fritzSmartPlugPtr->powerThreshold = _powerThreshold;
                fritzSmartPlugPtr->powerOffDelay = _powerOffDelay; 
                devices.push_back(fritzSmartPlugPtr);
            }
            else
            {
                if (_fritzboxPtr->errorCode() == TR064::TR064_CODE_ARRAYINDEXINVALID)
                {
                    _pollMillis = currentMillis;    
                    setState(SmartHomeState::Ready);
                }
                else
                {
                    _logger.logEvent(
                        "SmartHome: Discovery error %d '%s'. Retry in %d s...",
                        _fritzboxPtr->errorCode(),
                        _fritzboxPtr->errorDescription(),
                        SH_RETRY_DELAY);
                    errors++;
                    delay(SH_RETRY_DELAY * 1000);
                }
            }
            break;

        case SmartHomeState::Ready:
            if (currentMillis >= _pollMillis && devices.size() > 0)
            {
                _pollMillis += _pollInterval * 1000;
                updateDevice();
            }
            break;

        default:
            // Nothing to do
            break;        
    }
}


bool SmartHomeClass::updateDevice()
{
    time_t currentTime = time(nullptr);

    if (_currentDeviceIndex >= devices.size())
        _currentDeviceIndex = 0;

    TRACE("Updating device #%d...\n", _currentDeviceIndex);

    SmartDevice* smartDevicePtr = devices[_currentDeviceIndex];
    SmartDeviceState deviceStateBefore = smartDevicePtr->state;

    if (!smartDevicePtr->update(currentTime))
    {
        errors++;
        return false;
    }

    if (deviceStateBefore == SmartDeviceState::On && smartDevicePtr->state == SmartDeviceState::Off)
    {
        // Device switched off; update energy log
        energyLog.add(&smartDevicePtr->energyLogEntry);
        logEntriesToSync = std::max(logEntriesToSync + 1, SH_ENERGY_LOG_SIZE);
    }

    _currentDeviceIndex++;
    return true;
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
        state = SmartDeviceState::Off;
    }
    else
    {
        if (power >= powerThreshold)
        {
            if (state == SmartDeviceState::Off)
            {
                state = SmartDeviceState::On;
                energyLogEntry.reset(currentTime, energy);
            }
            energyLogEntry.update(currentTime, energy, power);
        }
        else if (currentTime > energyLogEntry.end + powerOffDelay)
        {
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
        _logger.logEvent(
            "FritzSmartPlug error %d '%s'",
            _fritzboxPtr->errorCode(),
            _fritzboxPtr->errorDescription());
        return false;
    }

    if (fields[0][1] != "CONNECTED")
    {
        state = SmartDeviceState::Unknown;
        return false;
    }

    if (fields[1][1] == "VALID")
    {
        if (fields[2][1] == "OFF")
            switchState = SmartDeviceState::Off;
        else if (fields[2][1] == "ON")
            switchState = SmartDeviceState::On;
        else
        {
            _logger.logEvent("FritzSmartPlug: SwitchState='%s'", fields[2][1].c_str());
            switchState = SmartDeviceState::Unknown;
        }
    }
    else
    {
        _logger.logEvent("FritzSmartPlug: SwitchIsValid='%s'", fields[1][1].c_str());
        switchState = SmartDeviceState::Unknown;
    }

    if (fields[3][1] != "VALID")
    {
        _logger.logEvent("FritzSmartPlug: MultimeterIsValid='%s'", fields[3][1].c_str());
        return false;
    }

    power = fields[4][1].toFloat() / 100;
    energy = fields[5][1].toFloat() / 1000;
    temperature = fields[6][1].toFloat() / 10;

    return SmartDevice::update(currentTime);
}
