#ifndef LED_H
#define LED_H

#include <stdint.h>

class LED
{
    public:
        virtual bool begin(bool on = true, uint8_t red = 0, uint8_t green = 0, uint8_t blue = 0xFF);
        virtual bool setOn(bool on);
        bool setOn() { return setOn(true); }
        bool setOff() { return setOn(false); }
        virtual bool setColor(uint8_t red, uint8_t green, uint8_t blue);
        bool isOn() { return _isOn; }
        void toggle() { setOn(!_isOn); }

    protected:
        uint8_t _pin = 0;
        uint8_t _red;
        uint8_t _green;
        uint8_t _blue;
        bool _isInitialized = false;
        bool _isOn;

        LED(uint8_t pin) : _pin(pin) {}
};


class SimpleLED : public LED
{
    public:
        SimpleLED(uint8_t pin, bool invert = false) : LED(pin), _invert(invert) {}
        bool begin(bool on = true, uint8_t red = 0, uint8_t green = 0, uint8_t blue = 0xFF) override;
        bool setOn(bool on) override;

    protected:
        bool _invert = false;
};


class RGBLED : public LED
{
    public: 
        RGBLED(uint8_t pin) : LED(pin) {}
        bool begin(bool on = true, uint8_t red = 0, uint8_t green = 0, uint8_t blue = 0xFF) override;
        bool setOn(bool on) override;
}; 

#endif