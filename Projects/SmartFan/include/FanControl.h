#include <Arduino.h>
#include <Ticker.h>
#include <deque>
#include <TimeUtils.h>
#include <Tracer.h>

class FanControlClass
{
    public:
        uint8_t humidityThreshold = 0;
        uint8_t maxLevel = 0;
        uint32_t maxLevelDuration = 0;

        // Constructor
        FanControlClass(uint8_t dacPin, uint8_t adcPin)
        {
            _dacPin = dacPin;
            _adcPin = adcPin;
        }

        int8_t getLevel() { return _level; }
        time_t keepLevelUntil() { return _keepLevelUntil; }
        float getVoltage() { return getAdcMilliVolts() / _adcScale; }

        bool begin(float dacScale, float adcScale)
        {
            _dacScale = dacScale;
            _adcScale = adcScale;
            _ticker.attach(1.0F, tick, this);
            return setLevel(0);
        }

        void calibrate(float measuredVoltage, float& dacScale, float& adcScale)
        {
            _dacScale = float(_dacValue) / measuredVoltage;
            _adcScale = getAdcMilliVolts() / measuredVoltage;
            dacScale = _dacScale;
            adcScale = _adcScale;
        }

        bool setLevel(int8_t percentage, uint32_t duration = 0)
        {
            TRACE("FanControlClass::setLevel(%d, %d)\n", percentage, duration);

            if ((_currentTime < _keepLevelUntil) && (percentage < _level)) 
            {
                TRACE("Requested fan level is below %d\n", _level);
                return true;
            }
            if (duration > 0)
            {
                _keepLevelUntil = _currentTime + duration;
                TRACE("Keep level until %s\n", formatTime("%T", _keepLevelUntil));
            }

            _level = percentage;
            _dacValue = (percentage >= 0)
                ? std::min((0.095F * percentage + 0.5F) * _dacScale, 255.0F)
                : 0;
            TRACE("DAC=%d\n", _dacValue);
            return dacWrite(_dacPin, _dacValue);
        }

        bool setMaxLevel() { return setLevel(maxLevel, maxLevelDuration); }

        bool setHumidity(float humidity, float baseline)
        {
            TRACE("FanControlClass::setHumidity(%0.1f, %0.1f)\n", humidity, baseline);

            if (humidity > humidityThreshold)
                return setMaxLevel();

            // Proportional control when humidity is above baseline but below threshold
            uint8_t percentage = std::max(100.0F * (humidity - baseline) / (float(humidityThreshold) - baseline), 0.0F);
            return (percentage != _level) ? setLevel(percentage) : true;
        }

    private:
        uint8_t _dacPin;
        uint8_t _adcPin;
        int8_t _level;
        uint8_t _dacValue;
        float _dacScale;
        float _adcScale;
        Ticker _ticker;
        time_t _currentTime = 0;
        time_t _keepLevelUntil = 0;

        float getAdcMilliVolts()
        {
            Tracer tracer("FanControl::getAdcMilliVolts");

            const int aggregations = 5;
            float aggregated = 0;
            for (int i = 0; i < aggregations; i++)
                aggregated += analogReadMilliVolts(_adcPin);
            return aggregated / aggregations; 
        }

        void tick()
        {
            _currentTime = time(nullptr);
            if ((_keepLevelUntil != 0) && (_currentTime >= _keepLevelUntil))
            {
                // Slowly decrease level after _keepLevelUntil is reached
                if (_level > 0)
                    setLevel(_level - 1);
                else
                    _keepLevelUntil = 0;
            }
        }

        static inline void tick(FanControlClass* instancePtr) { instancePtr->tick(); }
};
