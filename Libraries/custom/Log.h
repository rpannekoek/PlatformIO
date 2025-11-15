#ifndef LOG_H
#define LOG_H

#include <stdint.h>
#include <iterator>
#include <deque>
#include <PSRAM.h>

template<typename T>
class Log
{
    public:
        using iterator = typename std::deque<T*>::iterator;
        using const_iterator = typename std::deque<T*>::const_iterator;

        Log(uint16_t size) 
            : _size(size) {}

        ~Log() { clear(); }

        uint16_t size() const { return _size; }
        uint16_t count() const { return static_cast<uint16_t>(_pointers.size()); }

        void clear()
        {
            for (T* ptr : _pointers) delete ptr;
            _pointers.clear();
        }

        T* add(T* entryPtr)
        {
            // Note: takes ownership of the pointer passed in; the entry is deleted when expelled.
            if (!entryPtr) return nullptr;

            if (_pointers.size() == _size)
            {
                T* expelledPtr = _pointers.front();
                _pointers.pop_front();
                delete expelledPtr;
            }

            _pointers.push_back(entryPtr);

            return entryPtr;
        }

        iterator begin() { return _pointers.begin(); }
        iterator end() { return _pointers.end(); }
        const_iterator begin() const { return _pointers.begin(); }
        const_iterator end() const { return _pointers.end(); }

        iterator at(int16_t index)
        {
            if (index >= 0)
            {
                if (index >= _pointers.size()) return end();
                return begin() + index;
            }
            else
            {
                // Negative index means from end.
                if (-index >= _pointers.size()) return begin();
                return end() + index;
            }
        }

    private:
        uint16_t _size = 0;
        std::deque<T*> _pointers;
};

template<typename T>
class StaticLog
{
    public:
        StaticLog(uint16_t size, MemoryType memoryType = MemoryType::External)
            : _memoryType(memoryType), _size(size) {}

        ~StaticLog()
        {
            if (_entries) free(_entries);
        }

        int size() const { return _size; }
        uint16_t count() const { return _count; }

        void clear()
        {
            _start = 0;
            _end = 0;
            _count = 0;
            _iterator = 0;
        }

        T* add(const T* entryPtr)
        {
            if (!_entries) _entries = Memory::allocate<T>(_size, _memoryType);

            if ((_end == _start) && (_count != 0))
                _start = (_start + 1) % _size;
            else
                _count++;

            T* newEntryPtr = _entries + _end;
            memcpy(newEntryPtr, entryPtr, sizeof(T));                

            _end = (_end + 1) % _size;

            return newEntryPtr;
        }

        T* add(const T entry)
        {
            return add(&entry);
        }

        [[deprecated("Use iterator instead")]]
        T* getFirstEntry()
        {
            if (!_entries || !_count) return nullptr;
            return _entries + _iterator;
        }

        [[deprecated("Use iterator instead")]]
        T* getEntryFromEnd(uint16_t n)
        {
            if (!_entries || (n == 0) || (n > _count))
                return nullptr;

            if (_end < n)
                _iterator = _end + _size - n;
            else
                _iterator = _end - n;

            return _entries + _iterator;
        }

        [[deprecated("Use iterator instead")]]
        T* getNextEntry()
        {
            _iterator = (_iterator + 1) % _size;
            if (!_entries || _iterator == _end)
                return nullptr;
            return _entries + _iterator;
        }

        // Iterator support for range-based for loops
        class iterator
        {
            public:
                using iterator_category = std::forward_iterator_tag;
                using value_type = T;
                using difference_type = std::ptrdiff_t;
                using pointer = T*;
                using reference = T&;

                iterator(StaticLog<T>& log, uint16_t pos, uint16_t count)
                    : _log(log), _pos(pos), _count(count) {}

                pointer operator->() const 
                {
                    return &operator*();
                }

                reference operator*() const
                {
                    static T _dummy{};
                    return _log._entries ? _log._entries[_pos] : _dummy;
                }

                iterator& operator++() // pre-increment (used by range-based for loops)
                {
                    if (_count != 0)
                    {
                        _pos = (_pos + 1) % _log._size;
                        _count--;
                    }
                    return *this;
                }

                iterator operator++(int) // post-increment (for completeness)
                {
                    iterator pre = *this;
                    ++*this;
                    return pre;
                }

                bool operator==(const iterator& other) const
                {
                    return (_pos == other._pos) && (_count == other._count);
                }

                bool operator!=(const iterator& other) const 
                { 
                    return (_pos != other._pos) || (_count != other._count);
                }

                uint16_t remaining() const
                {
                    return _count;
                }

            private:
                StaticLog<T>& _log;
                uint16_t _pos;
                uint16_t _count;
        };

        iterator begin()
        {
            return iterator(*this, _start, _count);
        }

        iterator end()
        {
            return iterator(*this, _end, 0);
        }

        iterator at(int16_t index)
        {
            uint16_t pos;
            uint16_t count;
            if (index >= 0)
            {
                if (index >= _count) return end();
                pos = (_start + index) % _size;
                count = _count - index;
            }
            else
            {
                // Negative index => from end
                if (-index >= _count) return begin();
                pos = (-index > _end) 
                    ? _end + _size + index 
                    : _end + index;
                count = -index;
            }
            return iterator(*this, pos, count);
        }

    private:
        MemoryType _memoryType;
        uint16_t _size;
        uint16_t _start = 0;
        uint16_t _end = 0;
        uint16_t _count = 0;
        uint16_t _iterator = 0;
        T* _entries = nullptr;
};

class StringLog
{
    public:
        StringLog(uint16_t size, uint16_t entrySize, MemoryType memoryType = MemoryType::External)
            : _memoryType(memoryType), _size(size), _entrySize(entrySize) {}

        ~StringLog()
        {
            if (_entries) free(_entries);
        }

        uint16_t size() const { return _size; }
        uint16_t count() const { return _count; }

        void clear()
        {
            _start = 0;
            _end = 0;
            _count = 0;
            _iterator = 0;
        }

        const char* add(const char* entry)
        {
            if (!_entries) _entries = Memory::allocate<char>(_entrySize * _size, _memoryType);

            if ((_end == _start) && (_count != 0))
                _start = (_start + 1) % _size;
            else
                _count++;

            char* newEntryPtr = _entries + _end * _entrySize;
            _end = (_end + 1) % _size;

            strncpy(newEntryPtr, entry, _entrySize);
            newEntryPtr[_entrySize - 1] = 0;               

            return newEntryPtr;
        }

        [[deprecated("Use iterator instead")]]
        const char* getFirstEntry()
        {
            _iterator = _start;
            if (_count == 0)
                return nullptr;
            else
                return _entries + _iterator * _entrySize;
        }

        [[deprecated("Use iterator instead")]]
        const char* getEntryFromEnd(uint16_t n)
        {
            if ((n == 0) || (n > _count))
                return nullptr;
            
            if (_end < n)
                _iterator = _end + _size - n;
            else
                _iterator = _end - n;

            return _entries + _iterator * _entrySize;
        }

        [[deprecated("Use iterator instead")]]
        const char* getNextEntry()
        {
            _iterator = (_iterator + 1) % _size;
            if (_iterator == _end)
                return nullptr;
            else 
                return _entries + _iterator * _entrySize;
        }

        // Iterator support for range-based for loops
        class iterator
        {
            public:
                using iterator_category = std::forward_iterator_tag;
                using value_type = const char*;
                using difference_type = std::ptrdiff_t;
                using pointer = const char*;
                using reference = const char*;

                iterator(StringLog& log, uint16_t pos, uint16_t count)
                    : _log(log), _pos(pos), _count(count) {}

                pointer operator->() const
                {
                    return _log._entries ? (_log._entries + _pos * _log._entrySize) : nullptr;
                }

                reference operator*() const
                {
                    return _log._entries ? (_log._entries + _pos * _log._entrySize) : nullptr;
                }

                iterator& operator++() // Pre-increment (used by range-based loop)
                {
                    if (_count != 0)
                    {
                        _pos = (_pos + 1) % _log._size;
                        _count--;
                    }
                    return *this;
                }

                iterator operator++(int) // Post-increment (for completeness)
                {
                    iterator pre = *this;
                    ++*this;
                    return pre;
                }

                bool operator==(const iterator& other) const
                {
                    return (_pos == other._pos) && (_count == other._count);
                }

                bool operator!=(const iterator& other) const 
                { 
                    return (_pos != other._pos) || (_count != other._count);
                }

                uint16_t remaining() const
                {
                    return _count;
                }

            private:
                StringLog& _log;
                uint16_t _pos;
                uint16_t _count;
        };


        iterator begin()
        {
            return iterator(*this, _start, _count);
        }

        iterator end()
        {
            return iterator(*this, _end, 0);
        }

        iterator at(int16_t index)
        {
            uint16_t pos;
            uint16_t count;
            if (index >= 0)
            {
                if (index >= _count) return end();
                pos = (_start + index) % _size;
                count = _count - index;
            }
            else
            {
                // Negative index => from end
                if (-index >= _count) return begin();
                pos = (-index > _end) 
                    ? _end + _size + index 
                    : _end + index;
                count = -index;
            }
            return iterator(*this, pos, count);
        }

    protected:
        MemoryType _memoryType;
        uint16_t _size;
        uint16_t _entrySize;
        uint16_t _start = 0;
        uint16_t _end = 0;
        uint16_t _count = 0;
        uint16_t _iterator = 0; 
        char* _entries = nullptr;
};

#endif