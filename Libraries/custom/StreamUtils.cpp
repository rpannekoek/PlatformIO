#include <StreamUtils.h>
#include <Arduino.h>
#include <Tracer.h>
#include <PSRAM.h>


bool awaitDataAvailable(Stream& stream, int amount, int timeoutMs)
{
    if (timeoutMs == 0) timeoutMs = stream.getTimeout();
    while (stream.available() < amount)
    {
        delay(10);
        timeoutMs -= 10;
        if (timeoutMs < 0) return false;
    }
    return true;
}


MemoryStream::MemoryStream(size_t size, MemoryType memoryType)
    : _memoryType(memoryType)
{
    allocateBuffer(size + 1); // Keep room for string terminator
}


MemoryStream::MemoryStream(const String& str)
{
    size_t length = str.length();
    allocateBuffer(length + 1); // Keep room for string terminator
    memcpy(_buffer, str.c_str(), length);
    _writePos = length;
    _buffer[_writePos] = 0;
}


MemoryStream::~MemoryStream()
{
    TRACE(F("MemoryStream::~MemoryStream() free %p\n"), _buffer);
    free(_buffer);
}


void MemoryStream::allocateBuffer(size_t size)
{
    _buffer = Memory::allocate<uint8_t>(size, _memoryType);
    _bufferSize = size;
}


int MemoryStream::available()
{
    return _readPos < _writePos;
}


int MemoryStream::peek()
{
    return available() ? _buffer[_readPos] : -1;
}


int MemoryStream::read()
{
    return available() ? _buffer[_readPos++] : -1;
}


size_t MemoryStream::write(const uint8_t* buffer, size_t size)
{
    if (_writePos + size >= _bufferSize)
    {
        uint8_t* oldBuffer = _buffer;
        size_t oldBufferSize = _bufferSize;
        allocateBuffer(_bufferSize * 2);
        memcpy(_buffer, oldBuffer, oldBufferSize);
        free(oldBuffer);
    }

    memcpy(_buffer + _writePos, buffer, size);
    _writePos += size;
    _buffer[_writePos] = 0;

    return size;
}

