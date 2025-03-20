#include <Arduino.h>
#include <Tracer.h>
#include "IEC61851ControlPilot.h"

constexpr uint32_t PWM_FREQ = 1000;
constexpr uint8_t PWM_RESOLUTION_BITS = 8;
constexpr int OVERSAMPLING = 5;

#ifdef DEBUG_ESP_PORT
constexpr uint32_t STATUS_UPDATE_INTERVAL_MS = 5000;
#else
constexpr uint32_t STATUS_UPDATE_INTERVAL_MS = 1000;
#endif

const char* _statusNames[] =
{
    "Not Connected",
    "Connected",
    "Charging",
    "Charging (ventilated)",
    "No power"
};


const char* IEC61851ControlPilot::getStatusName()
{
    return _statusNames[static_cast<int>(_status)];
}


IEC61851ControlPilot::IEC61851ControlPilot(uint8_t outputPin, uint8_t inputPin, uint8_t feedbackPin, float maxCurrent)
{
    _outputPin = outputPin;
    _inputPin = inputPin;
    _feedbackPin = feedbackPin;
    _maxCurrent = maxCurrent;
}


bool IEC61851ControlPilot::begin(float scale)
{
    Tracer tracer("IEC61851ControlPilot::begin");

    _dutyCycle = 0;
    _scale = scale; 

    pinMode(_inputPin, ANALOG);
    pinMode(_outputPin, OUTPUT);
    pinMode(_feedbackPin, INPUT);

    digitalWrite(_outputPin, 0); // 0 V

    analogSetPinAttenuation(_inputPin, ADC_11db);

    return true;
}


int IEC61851ControlPilot::calibrate()
{
    Tracer tracer("IEC61851ControlPilot::calibrate");

    bool isOff = _dutyCycle == 0; 
    if (isOff) setReady(); // Temporary set 12V output

    int standbyLevel;
    int retries = 0;
    do
    {
        delay(100);
        standbyLevel = 0;
        for (int i = 0; i < OVERSAMPLING; i++)
            standbyLevel += analogReadMilliVolts(_inputPin);
        standbyLevel /= OVERSAMPLING;
    }
    while ((standbyLevel < MIN_CP_STANDBY_LEVEL) && (retries++ < 10));

    if (standbyLevel >= MIN_CP_STANDBY_LEVEL)
    {
        _scale = 12.0F / standbyLevel;
        TRACE("Standby level: %d => scale = %0.4f\n", standbyLevel, _scale);
    }
 
    if (isOff) setOff();

    return standbyLevel;
}


void IEC61851ControlPilot::setOff()
{
    Tracer tracer("IEC61851ControlPilot::setOff");

    if (_dutyCycle > 0 && _dutyCycle < 1)
    {
        ledcDetach(_outputPin);
        pinMode(_outputPin, OUTPUT);
    }

    digitalWrite(_outputPin, 0); // 0 V
    _dutyCycle = 0;
}


void IEC61851ControlPilot::setReady()
{
    Tracer tracer("IEC61851ControlPilot::setReady");

    if (_dutyCycle > 0 && _dutyCycle < 1)
    {
        ledcDetach(_outputPin);
        pinMode(_outputPin, OUTPUT);
    }

    digitalWrite(_outputPin, 1); // 12 V
    _dutyCycle = 1;
}


float IEC61851ControlPilot::setCurrentLimit(float ampere)
{
    Tracer tracer("IEC61851ControlPilot::setCurrentLimit");

    if (_dutyCycle == 0 || _dutyCycle == 1)
    {
        if ( !ledcAttach(_outputPin, PWM_FREQ, PWM_RESOLUTION_BITS))
            TRACE("Unable to attach LEDC to pin %u\n", _outputPin);
    }

    ampere = std::min(std::max(ampere, 6.0F), _maxCurrent);
    _dutyCycle =  ampere / 60.0F;
    uint32_t duty = static_cast<uint32_t>(std::round(_dutyCycle * 256));

    ledcWrite(_outputPin, duty);

    TRACE(
        "Set current limit %0.1f A. Duty cycle %0.0f %% (%d)\n",
        ampere,
        _dutyCycle * 100,
        duty);

    return ampere;
}


float IEC61851ControlPilot::getVoltage()
{
    uint32_t originalDuty = 0;
    if (_dutyCycle > 0 && _dutyCycle < 1)
    {
        // Can't measure voltage is duty cycle is very low   
        originalDuty = ledcRead(_outputPin);
        if (originalDuty < 32) ledcWrite(_outputPin, 32);

        // Wait for CP output low -> high transition
        int i = 0;
        while (digitalRead(_feedbackPin) == 1)
        {
            if (i++ == 150)
            {
                TRACE("Timeout waiting for CP low\n");
                return -1;
            }
            delayMicroseconds(10);
        }
        while (digitalRead(_feedbackPin) == 0)
        {
            if (i++ == 300)
            {
                TRACE("Timeout waiting for CP high\n");
                return -1;
            }
            delayMicroseconds(10);
        }
        delayMicroseconds(5); // Just switched to high; give signal some time to settle
    }

    float voltage = _scale * analogReadMilliVolts(_inputPin);

    if ((_dutyCycle > 0) && (_dutyCycle < 0.125))
        ledcWrite(_outputPin, originalDuty);

    return voltage;
}


bool IEC61851ControlPilot::awaitStatus(ControlPilotStatus status, int timeoutMs)
{
    while (_status != status && timeoutMs > 0)
    {
        delay(10);
        timeoutMs -= 10;
        determineStatus();
    }
    return _status == status;
}


ControlPilotStatus IEC61851ControlPilot::getStatus()
{
    if (millis() >= _nextStatusUpdateMillis) 
    {
        determineStatus();
        TRACE("CP: %s\n", getStatusName());
    }
    return _status;
}


void IEC61851ControlPilot::setTestStatus(ControlPilotStatus status)
{
    _status = status;
    _nextStatusUpdateMillis = millis() + 300 * 1000;

    TRACE("IEC61851ControlPilot::setTestStatus(%s)\n", getStatusName());
}


void IEC61851ControlPilot::determineStatus() 
{
    float voltage;
    int retries = 3;
    do
    {
        voltage = getVoltage();
        if (voltage == 0.0F && _dutyCycle > 0  && retries > 0)
        {
            TRACE("Measured 0 V with duty cycle %0.0f. Retrying...\n", _dutyCycle * 100);
            voltage = -1;
        }
    }
    while (voltage < 0 && retries-- > 0);

    if (voltage > 10.5)
        _status =  ControlPilotStatus::Standby;
    else if (voltage > 7.5)
        _status = ControlPilotStatus::VehicleDetected;
    else if (voltage > 4.5)
        _status = ControlPilotStatus::Charging;
    else if (voltage > 1.5)
        _status = ControlPilotStatus::ChargingVentilated;
    else
        _status = ControlPilotStatus::NoPower;

    _nextStatusUpdateMillis = millis() + STATUS_UPDATE_INTERVAL_MS;
}
