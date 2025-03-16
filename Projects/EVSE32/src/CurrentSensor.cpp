#include <Tracer.h>
#include "CurrentSensor.h"

constexpr uint32_t PERIOD_MS = 20; // 50 Hz
constexpr uint32_t SAMPLE_FREQ = 5000; // Hz
constexpr uint32_t SAMPLES_PER_PERIOD = SAMPLE_FREQ * PERIOD_MS / 1000;
constexpr uint32_t ADC_FRAME_SIZE = SAMPLES_PER_PERIOD * SOC_ADC_DIGI_RESULT_BYTES;

uint8_t _adcFrame[ADC_FRAME_SIZE];


CurrentSensor::CurrentSensor(uint8_t pin, size_t bufferSize)
{
    _pin = pin;
    _sampleBufferSize = bufferSize;
    _sampleBufferPtr = new int16_t[bufferSize];
}


bool CurrentSensor::begin(float scale)
{
    Tracer tracer("CurrentSensor::begin");

    _scale = scale;

    pinMode(_pin, ANALOG);

    int8_t adcChannel = digitalPinToAnalogChannel(_pin);
    if (adcChannel < 0)
    {
        TRACE("No ADC channel for pin %d\n", _pin);
        return false;
    }

    adc_continuous_handle_cfg_t adcHandleConfig =
    {
        .max_store_buf_size = ADC_FRAME_SIZE * 2,
        .conv_frame_size = ADC_FRAME_SIZE
    };
    esp_err_t err = adc_continuous_new_handle(&adcHandleConfig, &_adcContinuousHandle);
    if (err != ESP_OK)
    {
        TRACE("adc_continuous_new_handle returned %d\n", err);
        return false;
    }

    adc_digi_pattern_config_t adcPatternConfig =
    {
        .atten = ADC_11db,
        .channel = static_cast<uint8_t>(adcChannel),
        .unit = ADC_UNIT_1,
        .bit_width = 12
    };
    adc_continuous_config_t adcConfig =
    {
        .pattern_num = 1,
        .adc_pattern = &adcPatternConfig,
        .sample_freq_hz = SAMPLE_FREQ,
        .conv_mode = ADC_CONV_SINGLE_UNIT_1,
        .format = ADC_DIGI_OUTPUT_FORMAT_TYPE2
    };
    err = adc_continuous_config(_adcContinuousHandle, &adcConfig);
    if (err != ESP_OK)
    {
        TRACE("adc_continuous_config returned %d\n", err);
        return false;
    }

    return true;
}


bool CurrentSensor::measure(uint16_t periods)
{
    Tracer tracer("CurrentSensor::measure");

    _sampleIndex = 0;

    uint16_t maxPeriods = _sampleBufferSize / SAMPLES_PER_PERIOD;
    periods = std::min(periods, maxPeriods);
    TRACE("Measuring %d periods...\n", periods);

    esp_err_t err = adc_continuous_start(_adcContinuousHandle);
    if (err != ESP_OK)
    {
        TRACE("adc_continuous_start returned %d\n", err);
        return false;
    }

    uint32_t total = 0;
    for (int p = 0; p < periods; p++)
    {
        uint32_t bytesRead;
        err = adc_continuous_read(_adcContinuousHandle, _adcFrame, ADC_FRAME_SIZE, &bytesRead, PERIOD_MS + 1);
        if (err != ESP_OK)
        {
            TRACE("adc_continuous_read returned %d\n", err);
            adc_continuous_stop(_adcContinuousHandle);
            return false;
        }
    
        for (int i = 0; i < bytesRead; i += SOC_ADC_DIGI_RESULT_BYTES)
        {
            adc_digi_output_data_t* adcSamplePtr = (adc_digi_output_data_t*)&_adcFrame[i];
            uint32_t adcSample = adcSamplePtr->type2.data;
            _sampleBufferPtr[_sampleIndex++] = adcSample;
            total += adcSample;
        }
    }
    _dc = (_sampleIndex == 0) ? 0 : total / _sampleIndex; // Average

    err = adc_continuous_stop(_adcContinuousHandle);
    if (err != ESP_OK)
    {
        TRACE("adc_continuous_stop returned %d\n", err);
        return false;
    }

    return _sampleIndex > 0;
}


float CurrentSensor::calibrateScale(float actualRMS)
{
    Tracer tracer("CurrentSensor::calibrateScale");

    float measuredRMS = getRMS();
    if ((measuredRMS > 0) && (measuredRMS < 100))
    {
        _scale *= actualRMS / measuredRMS;
        TRACE("Measured %0.3f A, Actual %0.3f A => scale = %0.3f\n", measuredRMS, actualRMS, _scale);
    }
    else
    {
        _scale = 0.016;
        TRACE("Measured RMS out of range. Reset scale.\n");
    }
 
    return _scale;
}


float CurrentSensor::getPeak()
{
    if (_sampleBufferPtr == nullptr || _sampleIndex == 0)
        return 0.0F;

    int peak = 0;
    for (int i = 0; i < _sampleIndex; i++)
        peak = std::max(peak, std::abs(_sampleBufferPtr[i] - _dc));

    return _scale * peak;
}


float CurrentSensor::getRMS()
{
    if (_sampleBufferPtr == nullptr || _sampleIndex == 0)
        return 0.0F;

    double sumSquares = 0;
    for (int i = 0; i < _sampleIndex; i++)
        sumSquares += std::pow((_sampleBufferPtr[i] - _dc), 2);

    return _scale * sqrt(sumSquares / _sampleIndex);
}


void CurrentSensor::writeSampleCsv(Print& writeTo, bool raw)
{
    String csvHeader = raw ? "Raw, AC" : "I (A)";
    writeTo.println(csvHeader);

    for (uint16_t i = 0; i < _sampleIndex; i++)
    {
        if (raw)
            writeTo.printf("%d, %d\n", _sampleBufferPtr[i], _sampleBufferPtr[i] - _dc);
        else
            writeTo.printf("%0.3f\n", _scale * (_sampleBufferPtr[i] - _dc));
    }
}
