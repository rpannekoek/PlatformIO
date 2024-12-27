#ifndef CONSTANTS_H
#define CONSTANTS_H

#include <TimeUtils.h>

#ifdef ARDUINO_LOLIN_S2_MINI
constexpr uint8_t FAN_DAC_PIN = DAC2; // pin 18
constexpr uint8_t FAN_ADC_PIN = 3; // ADC1 CH2
#else
constexpr uint8_t FAN_DAC_PIN = DAC2; // pin 26 on D32 pro
constexpr uint8_t FAN_ADC_PIN = 32; // ADC1 CH4
#endif
constexpr uint8_t BME_SDA_PIN = SDA;
constexpr uint8_t BME_SCL_PIN = SCL;

constexpr int DEBUG_BAUDRATE = 115200;
constexpr size_t HTTP_RESPONSE_BUFFER_SIZE = 12 * 1024;
constexpr size_t HTTP_CHUNK_SIZE = 8 * 1024;

constexpr uint32_t CALIBRATE_TIME = SECONDS_PER_MINUTE;
constexpr uint32_t IAQ_POLL_INTERVAL = 3; // seconds
constexpr uint32_t IAQ_SAMPLES_PER_MINUTE = SECONDS_PER_MINUTE / IAQ_POLL_INTERVAL;

constexpr int FAN_LOG_SIZE = 150;
constexpr int FAN_LOG_PAGE_SIZE = 50;

constexpr int MAX_EVENT_LOG_SIZE = 50;

constexpr int FTP_RETRY_INTERVAL = 15 * SECONDS_PER_MINUTE;
constexpr int FTP_TIMEOUT_MS = 5000;

const char* ContentTypeHtml = "text/html;charset=UTF-8";
const char* ContentTypeText = "text/plain";
const char* ContentTypeJson = "application/json";
const char* ButtonClass = "button";

#endif
