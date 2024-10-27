#include <Tracer.h>
#include "SmartHome.h"

const char* _smartDeviceStateLabels[] = { "Disabled", "Off", "On" };


bool SmartHomeClass::begin(float powerThreshold, uint32_t powerOffDelay, uint32_t pollInterval)
{
    _powerThreshold = powerThreshold;
    _powerOffDelay = powerOffDelay;
    _pollInterval = pollInterval;
    _pollDeviceIndex = 0;
}


bool SmartHomeClass::useFritzbox(const char* host, const char* user, const char* password)
{
    Tracer tracer("SmartHomeClass::useFritzbox", host);

    _fritzbox.debug_level = TR064::DEBUG_VERBOSE;
    _fritzbox.setServer(49000, host, user, password);
}


bool SmartHomeClass::discoverDevices()
{
    Tracer tracer("SmartHomeClass::discoverDevices");

    String params[][2] = {{"NewIndex", ""}};
    String fields[][2] = {{"NewAIN", ""}, {"NewDeviceName", ""}};

    devices.clear();
    for (int i = 0; i < MAX_SMART_DEVICES; i++)
    {
        params[0][1] = String(i);
        if (!_fritzbox.action("X_AVM-DE_Homeauto:1", "GetGenericDeviceInfos", params, 1, fields, 2))
        {
            return i != 0;
        }
        SmartDevice* smartDevicePtr = new FritzSmartPlug(fields[0][1], fields[1][1], _fritzbox);
        smartDevicePtr->powerThreshold = _powerThreshold;
        smartDevicePtr->powerOffDelay = _powerOffDelay; 
        devices.push_back(smartDevicePtr);
    }

    return false;
}


bool SmartHomeClass::update(time_t currentTime)
{
    if (currentTime < _lastPollTime + _pollInterval)
        return true;

    _lastPollTime = currentTime;

    if (_pollDeviceIndex >= devices.size())
        _pollDeviceIndex = 0;

    SmartDevice* smartDevicePtr = devices[_pollDeviceIndex++];
    return smartDevicePtr->update(currentTime);
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
    if (state == SmartDeviceState::Disabled)
        state = SmartDeviceState::Off;

    if (switchState == SmartDeviceState::Off)
    {
        state = SmartDeviceState::Off;
    }
    else
    {
        if (power >= powerThreshold)
        {
            state = SmartDeviceState::On;
            lastOn = currentTime;
        }
        else if (currentTime > lastOn + powerOffDelay)
        {
            state = SmartDeviceState::Off;
        }
    }

    return true;
}


bool FritzSmartPlug::update(time_t currentTime)
{
    Tracer tracer("FritzSmartPlug::update", id.c_str());

    String params[][2] = {{"NewAIN", id}};
    String fields[][2] = 
    {
        {"NewPresent", ""},
        {"NewSwitchState", ""},
        {"NewMultimeterPower", ""},
        {"NewMultimeterEnergy", ""},
        {"NewTemperatureCelcius", ""}
    };

    if (!_fritzbox.action("X_AVM-DE_Homeauto:1", "GetSpecificDeviceInfos", params, 1, fields, 5))
    {
        return false;
    }

    if (fields[0][1] != "CONNECTED")
    {
        state = SmartDeviceState::Disabled;
        return true;
    }

    switchState = fields[1][1] == "ON" ? SmartDeviceState::On : SmartDeviceState::Off;
    power = fields[2][1].toFloat() / 100;
    energy = fields[3][1].toFloat() / 1000;
    temperature = fields[4][1].toFloat() / 10;

    return SmartDevice::update(currentTime);
}
