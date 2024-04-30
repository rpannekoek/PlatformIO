#ifndef CONSTANTS_H
#define CONSTANTS_H

#include <TimeUtils.h>

constexpr int DEBUG_BAUDRATE = 115200;
const char* DEFAULT_DTU_SERIAL = "199990100000";

constexpr size_t MAX_REGISTERED_INVERTERS = 4;
constexpr size_t MAX_DC_CHANNELS_PER_INVERTER = 4;
constexpr size_t MAX_INVERTER_NAME_LENGTH = 16;

constexpr uint32_t POLL_INTERVAL_DAY = 6;
constexpr uint32_t POLL_INTERVAL_NIGHT = 5 * SECONDS_PER_MINUTE;
constexpr uint32_t TODAY_LOG_INTERVAL = SECONDS_PER_HOUR / 2;

constexpr int POWER_LOG_SIZE = 200;
constexpr int POWER_LOG_PAGE_SIZE = 50;
constexpr int POWER_LOG_AGGREGATIONS = SECONDS_PER_MINUTE / POLL_INTERVAL_DAY;
constexpr float POWER_EQUALS_MARGIN = 1.0F;
constexpr float VOLTAGE_EQUALS_MARGIN = 0.5F;

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
