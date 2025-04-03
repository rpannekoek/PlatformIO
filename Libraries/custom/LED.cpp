#include <Arduino.h>
#include <LED.h>
#include <Tracer.h>

#ifdef ESP32
#include "esp32-hal-rgb-led.h"
#endif


bool LED::begin(bool on, uint8_t red, uint8_t green, uint8_t blue)
{
    if (_isInitialized) return false;
    _isInitialized = true;
    _red = red;
    _green = green;
    _blue = blue;
    return setOn(on);
}


bool LED::setOn(bool on)
{
    if (!_isInitialized || (!on && !_isOn)) return false;

    _isOn = on;
    return true;
}


bool LED::setColor(uint8_t red, uint8_t green, uint8_t blue)
{
    if ((red == _red) && (green == _green) && (blue == _blue))
        return false;

    _red = red;
    _green = green;
    _blue = blue;
    return setOn(red || green || blue);
}


bool SimpleLED::begin(bool on, uint8_t red, uint8_t green, uint8_t blue)
{
    TRACE(F("SimpleLED::begin(%d)\n"), on);
    pinMode(_pin, OUTPUT);
    return LED::begin(on, red, green, blue);
}


bool SimpleLED::setOn(bool on)
{
    if (LED::setOn(on))
    {
        digitalWrite(_pin, on ^ _invert);
        return true;
    }
    else
        return false;
}


bool RGBLED::begin(bool on, uint8_t red, uint8_t green, uint8_t blue)
{
    TRACE(F("RGBLED::begin(%d, %d, %d)\n"), red, green, blue);
    return LED::begin(on, red, green, blue);
}


bool RGBLED::setOn(bool on)
{
    if (!LED::setOn(on)) return false;
#ifdef ESP32
#if (ESP_ARDUINO_VERSION_MAJOR == 2)
    if (on)
#ifdef LED_RGB    
        neopixelWrite(_pin,  _red, _green, _blue);
#else
        neopixelWrite(_pin, _green, _red, _blue);
#endif
    else
        neopixelWrite(_pin, 0, 0, 0);
#else
    if (on)
        rgbLedWrite(_pin, _red, _green, _blue);
    else
        rgbLedWrite(_pin, 0, 0, 0);
#endif
    return true;
#else
    return false;
#endif
}

