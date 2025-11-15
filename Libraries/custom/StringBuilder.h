#ifndef STRINGBUILDER_H
#define STRINGBUILDER_H

#include <stdlib.h>
#include <WString.h>
#include <Print.h>
#include <PSRAM.h>

class StringBuilder : public Print
{
  public:
    StringBuilder(size_t size, MemoryType memoryType = MemoryType::Auto)
        :  _memoryType(memoryType), _capacity(size) {}

    ~StringBuilder()
    {
        if (_buffer) free(_buffer);
    }

    [[deprecated("Specify memory type in constructor")]]
    bool usePSRAM() { _memoryType = MemoryType::External; return true; }

    void clear();
    void printf(const __FlashStringHelper* fformat, ...);

    // Overrides for virtual Print methods
    virtual size_t write(uint8_t);
    virtual size_t write(const uint8_t *buffer, size_t size);

    size_t length() const { return _length; }
    const char* c_str() const { return _buffer ? _buffer : ""; }
    operator const char*() const { return c_str(); }

  protected:
    MemoryType _memoryType;
    size_t _capacity;
    size_t _space = 0;
    size_t _length = 0;
    char* _buffer = nullptr;
    
    void update_length(size_t additional);
};

#endif