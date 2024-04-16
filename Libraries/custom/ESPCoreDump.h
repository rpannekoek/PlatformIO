#ifndef ESP_COREDUMP_H
#define ESP_COREDUMP_H

#include <Print.h>

#ifdef ESP32
#include <esp_core_dump.h>
#endif

extern bool writeCoreDump(Print& output);

#endif