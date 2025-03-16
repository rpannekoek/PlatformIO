#include <stdint.h>
#include <esp_adc/adc_continuous.h>

class CurrentSensor
{
    public:
        CurrentSensor(uint8_t pin, size_t bufferSize = 1024);

        bool begin(float scale);
        float calibrateScale(float actualRMS);
        bool measure(uint16_t periods = 5);
        uint16_t getSampleCount() { return _sampleIndex; }

        float getPeak();
        float getRMS();
        uint16_t getDC() { return _dc; }

        void writeSampleCsv(Print& writeTo, bool raw);

    private:
        uint8_t _pin;
        uint16_t _sampleBufferSize;
        uint16_t _sampleIndex;
        int16_t* _sampleBufferPtr = nullptr;
        int16_t _dc;
        float _scale;
        adc_continuous_handle_t _adcContinuousHandle;
};