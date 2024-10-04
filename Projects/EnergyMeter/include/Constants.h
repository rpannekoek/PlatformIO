#ifndef CONSTANTS_H
#define CONSTANTS_H

#include <TimeUtils.h>

constexpr int DEBUG_BAUDRATE = 115200;

constexpr uint32_t POLL_INTERVAL = SECONDS_PER_MINUTE;
constexpr uint32_t TODAY_LOG_INTERVAL = SECONDS_PER_HOUR / 2;

constexpr int POWER_LOG_SIZE = 200;
constexpr int POWER_LOG_PAGE_SIZE = 50;
constexpr int POWER_LOG_AGGREGATIONS = SECONDS_PER_MINUTE / POLL_INTERVAL;
constexpr float POWER_EQUALS_MARGIN = 1.0F;

constexpr int MAX_EVENT_LOG_SIZE = 50;
constexpr int MAX_BAR_LENGTH = 50;

constexpr int FTP_RETRY_INTERVAL = 15 * SECONDS_PER_MINUTE;
constexpr int FTP_TIMEOUT_MS = 5000;

constexpr uint8_t NRF_CS_PIN = 16; 
constexpr uint8_t NRF_CE_PIN = 39;
constexpr uint8_t NRF_IRQ_PIN = 33;
constexpr uint8_t NRF_MISO = 35;
constexpr uint8_t NRF_MOSI = 18;
constexpr uint8_t NRF_SCK = 37;

#define TIMEFRAME_PARAM "timeframe"
#define INVERTER_PARAM "inverter"
#define CHANNEL_PARAM "channel"

const char* ContentTypeHtml = "text/html;charset=UTF-8";
const char* ContentTypeText = "text/plain";
const char* ButtonClass = "button";

#endif
