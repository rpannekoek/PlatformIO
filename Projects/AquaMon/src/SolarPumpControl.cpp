#include <Arduino.h>
#include <TimeUtils.h>
#include <Tracer.h>
#include "SolarPumpControl.h"

constexpr uint32_t PWM_RANGE = 255; // 8-bit PWM range
constexpr float PWM_OFF = 0.0F; // 0% = no PWM
constexpr float PWM_LOW = 0.1F; // 10% = pump low
constexpr float PWM_START = 0.15F; // 15% = start of slope

bool SolarPumpControl::begin()
{
    Tracer tracer(F("SolarPumpControl::begin"));

    pinMode(_pwmPin, OUTPUT);
    analogWriteFreq(500); // Hz
    analogWriteRange(PWM_RANGE);
    return true;
}


void SolarPumpControl::control(
    int solarDeltaT,
    int pumpOnDeltaT,
    int pumpOffDeltaT,
    int pwmDeltaT,
    int pwmChangeRatePct)
{
    if (solarDeltaT < pumpOffDeltaT)
    {
        // Pump is off. Turn PWM off too.
        _dutyCycle = PWM_OFF;
        _targetDutyCycle = PWM_OFF;
    }
    else if ((solarDeltaT <= pumpOnDeltaT) && (_dutyCycle == PWM_OFF))
    {
        // Pump is not on yet. Turn PWM on to prepare for start.
        _dutyCycle = PWM_LOW;
        _targetDutyCycle = PWM_LOW;
    }
    else if ((solarDeltaT > pumpOnDeltaT) || (_dutyCycle >= PWM_START))
    {
        // Pump is on. Control speed with PWM.
        float p = std::min(float(solarDeltaT - pumpOffDeltaT) / pwmDeltaT, 1.0F);
        _targetDutyCycle = PWM_START + 0.75F * p; // PWM should be between 15% and 90% duty cycle

        if (_dutyCycle < PWM_START) 
            _dutyCycle = PWM_START;
        else
        {
            // Slowly increase duty cycle towards target
            float pwmChangeRate = float(pwmChangeRatePct) / 100;
            _dutyCycle += std::min(_targetDutyCycle - _dutyCycle, pwmChangeRate); 
        }
    }

    analogWrite(_pwmPin, roundf(_dutyCycle * PWM_RANGE));
}


void SolarPumpControl::updateLog(time_t time, int solarDeltaT)
{
    SolarLogEntry newSolarLogEntry;
    newSolarLogEntry.deltaT = solarDeltaT;
    newSolarLogEntry.dutyCycle = _dutyCycle;
    newSolarLogEntry.targetDutyCycle = _targetDutyCycle;

    if (_lastSolarLogEntryPtr == nullptr || !newSolarLogEntry.equals(_lastSolarLogEntryPtr))
    {
        newSolarLogEntry.time = time;
        _lastSolarLogEntryPtr = Log.add(&newSolarLogEntry);
    }
}


void SolarPumpControl::writeStateRow(HtmlWriter& html)
{
    html.writeRowStart();
    html.writeHeaderCell(F("PWM"));
    html.writeCell("%0.0f/%0.0f %%", _dutyCycle * 100, _targetDutyCycle * 100);
    html.writeCellStart(F("graph fill"));
    html.writeMeterDiv(_dutyCycle, 0.0F, 1.0F, F("pwmBar"));
    html.writeCellEnd();
    html.writeRowEnd();
}
