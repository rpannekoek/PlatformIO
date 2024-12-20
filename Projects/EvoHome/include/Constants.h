#ifndef CONSTANTS_H
#define CONSTANTS_H

#include <TimeUtils.h>

constexpr int DEBUG_BAUDRATE = 115200;

constexpr int MAX_EVENT_LOG_SIZE = 50;
constexpr int MAX_BAR_LENGTH = 50;

constexpr size_t RAMSES_PACKET_LOG_SIZE = 100;
constexpr size_t PAGE_SIZE = 50;
constexpr size_t HTTP_CHUNK_SIZE = 8 * 1024;

constexpr int FTP_RETRY_INTERVAL = 15 * SECONDS_PER_MINUTE;
constexpr int FTP_TIMEOUT_MS = 5000;

#ifdef ARDUINO_LOLIN_D32
constexpr int8_t CC1101_CSN_PIN = SS;
constexpr int8_t CC1101_SCK_PIN = SCK;
constexpr int8_t CC1101_MISO_PIN = MISO;
constexpr int8_t CC1101_MOSI_PIN = MOSI;
constexpr int8_t CC1101_GDO2_PIN = RX;
constexpr int8_t CC1101_GDO0_PIN = TX;
#else
constexpr int8_t CC1101_CSN_PIN = 16;
constexpr int8_t CC1101_SCK_PIN = 37;
constexpr int8_t CC1101_MISO_PIN = 35;
constexpr int8_t CC1101_MOSI_PIN = 39;
constexpr int8_t CC1101_GDO2_PIN = 33;
constexpr int8_t CC1101_GDO0_PIN = 18;
#endif

const char* ContentTypeHtml = "text/html;charset=UTF-8";
const char* ContentTypeJson = "application/json";
const char* ContentTypeText = "text/plain";
const char* ButtonClass = "button";

#endif
