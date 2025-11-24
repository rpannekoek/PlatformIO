#ifndef STRINGBUILDER_H
#define STRINGBUILDER_H

#include <stdlib.h>
#include <WString.h>
#include <Print.h>
#include <PSRAM.h>

class StringBuilder : public Print
{
  public:
    StringBuilder(size_t capacity, MemoryType memoryType = MemoryType::Auto)
        :  _memoryType(memoryType), _capacity(capacity) {}

    ~StringBuilder()
    {
        if (_buffer) free(_buffer);
    }

    size_t capacity() const { return _capacity; }
    size_t length() const { return _length; }
    const char* c_str() const { return _buffer ? _buffer : ""; }
    operator const char*() const { return c_str(); }
    void onLowSpace(std::function<void(size_t)> fn) { _lowSpaceFn = fn; }

    void clear();
    void printf(const __FlashStringHelper* fformat, ...);
    
    // Overrides for virtual Print methods:
    size_t write(uint8_t) override;
    size_t write(const uint8_t *buffer, size_t size) override;

  protected:
    MemoryType _memoryType;
    size_t _capacity;
    size_t _space = 0;
    size_t _length = 0;
    char* _buffer = nullptr;
    std::function<void(size_t)> _lowSpaceFn = nullptr;
    
    void adjustLength(size_t additional);
};

#endif