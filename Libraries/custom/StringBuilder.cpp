#include <Arduino.h>
#include "StringBuilder.h"

StringBuilder::~StringBuilder()
{
    if (_buffer) 
    {
        TRACE(F("StringBuilder::~StringBuilder() free %p\n"), _buffer);
        free(_buffer);
    }
}



void StringBuilder::clear()
{
    if (!_buffer) _buffer = Memory::allocate<char>(_capacity, _memoryType);

    _buffer[0] = 0;
    _length = 0;
    _space = _capacity;
}


void StringBuilder::printf(const __FlashStringHelper* fformat, ...)
{
    if (!_buffer) clear();

    if (_space == 0) return;

    char* end = _buffer + _length;

    va_list args;
    va_start(args, fformat);
    size_t additional = vsnprintf_P(end, _space, (PGM_P) fformat, args);
    va_end(args);

    adjustLength(additional);
}


size_t StringBuilder::write(uint8_t data)
{
    return write(&data, 1);
}


size_t StringBuilder::write(const uint8_t* dataPtr, size_t size)
{
    if (!_buffer) clear();

    if (_space <= 1) return 0;
 
    if (size >= _space) size = (_space - 1);

    char* end = _buffer + _length;
    memcpy(end, dataPtr, size);
    end[size] = 0; 

    adjustLength(size);

    return size;
}


void StringBuilder::adjustLength(size_t additional)
{
    _length += additional;
    _space -= additional;

    if ((_space < 256) && _lowSpaceFn)
        _lowSpaceFn(_space);
}
