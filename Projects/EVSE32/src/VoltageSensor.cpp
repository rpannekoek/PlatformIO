#include <Tracer.h>
#include "VoltageSensor.h"


VoltageSensor::VoltageSensor(uint8_t pin)
{
    _pin = pin;
}


bool VoltageSensor::begin()
{
    Tracer tracer("VoltageSensor::begin");

    pinMode(_pin, INPUT);

    return true;
}


bool VoltageSensor::detectSignal(uint32_t sensePeriodMs)
{
    Tracer tracer("VoltageSensor::detectSignal");

    int pinLevel = digitalRead(_pin);

    for (int i = 0; i < sensePeriodMs; i++)
    {
        delay(1);
        if (digitalRead(_pin) != pinLevel) 
        {
            TRACE("%s edge detected.\n"), pinLevel ? "Falling" : "Rising";
            return true;
        }
    }

    TRACE("No edges detected in %d ms.\n", sensePeriodMs);
    return false;
}