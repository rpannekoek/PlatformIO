#ifndef CONSTANTS_H
#define CONSTANTS_H

#include <TimeUtils.h>

constexpr int DEBUG_BAUDRATE = 115200;

constexpr int MAX_EVENT_LOG_SIZE = 50;
constexpr int MAX_BAR_LENGTH = 50;

constexpr size_t RAMSES_PACKET_LOG_SIZE = 50;

constexpr int FTP_RETRY_INTERVAL = 15 * SECONDS_PER_MINUTE;
constexpr int FTP_TIMEOUT_MS = 5000;

// TODO
constexpr int8_t CC1101_SCK_PIN = 66;
constexpr int8_t CC1101_MISO_PIN = 66;
constexpr int8_t CC1101_MOSI_PIN = 66;
constexpr int8_t CC1101_CSN_PIN = 66;

const char* ContentTypeHtml = "text/html;charset=UTF-8";
const char* ContentTypeJson = "application/json";
const char* ContentTypeText = "text/plain";
const char* ButtonClass = "button";

#endif
