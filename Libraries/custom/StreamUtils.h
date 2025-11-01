#ifndef STREAM_UTILS_H
#define STREAM_UTILS_H

#include <Stream.h>

extern bool awaitDataAvailable(Stream& stream, int amount, int timeoutMs = 0);

class MemoryStream : public Stream
{
    public:
        MemoryStream(size_t size, bool usePSRAM = true);
        MemoryStream(const String& str);
        ~MemoryStream();

        size_t size() { return _writePos; }
        const char* c_str() { return (const char*)_buffer; }

        int available() override;
        int read() override;
        int peek() override;
        size_t write(const uint8_t* buffer, size_t size) override;
        size_t write(uint8_t data) override { return write(&data, 1); }

    private:
        bool _usePSRAM = false;
        uint8_t* _buffer;
        size_t _bufferSize;
        size_t _readPos = 0;
        size_t _writePos = 0;

        void allocateBuffer(size_t size);
};

#endif