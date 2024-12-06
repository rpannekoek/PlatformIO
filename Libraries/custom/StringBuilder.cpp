#include <Arduino.h>
#include "StringBuilder.h"

// Constructor
StringBuilder::StringBuilder(size_t size)
{
    _buffer = static_cast<char*>(malloc(size));
    _capacity = size;
    clear();
}

// Destructor
StringBuilder::~StringBuilder()
{
    free(_buffer);
}


bool StringBuilder::usePSRAM()
{
#ifdef BOARD_HAS_PSRAM
    free(_buffer);
    _buffer = static_cast<char*>(ps_malloc(_capacity));
    return true;
#else
    return false;
#endif    
}


void StringBuilder::clear()
{
    _buffer[0] = 0;
    _length = 0;
    _space = _capacity;
}


void StringBuilder::printf(const __FlashStringHelper* fformat, ...)
{
    if (_space == 0)
        return;

    char* end = _buffer + _length;

    va_list args;
    va_start(args, fformat);
    size_t additional = vsnprintf_P(end, _space, (PGM_P) fformat, args);
    va_end(args);

    update_length(additional);
}


size_t StringBuilder::write(uint8_t data)
{
    return write(&data, 1);
}


size_t StringBuilder::write(const uint8_t* dataPtr, size_t size)
{
    if (_space <= 1) 
        return 0;
 
    if (size >= _space)
        size = (_space - 1);

    char* end = _buffer + _length;
    memcpy(end, dataPtr, size);
    end[size] = 0; 

    update_length(size);

    return size;
}


void StringBuilder::update_length(size_t additional)
{
    _length += additional;
    _space -= additional;
}
