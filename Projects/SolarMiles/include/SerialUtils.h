#include <stdlib.h>
#include <stdio.h>
#include <string.h>

constexpr size_t SERIAL_LENGTH = 12;

uint64_t parseSerial(const char* str)
{
    return (strlen(str) == SERIAL_LENGTH) ? strtoull(str, nullptr, 16) : 0;
}


const char* formatSerial(uint64_t serial)
{
    static char result[16];
    snprintf(result, sizeof(result), "%0lX%08lX",
        ((uint32_t)((serial >> 32) & 0xFFFFFFFF)),
        ((uint32_t)(serial & 0xFFFFFFFF)));
    return result;
}
