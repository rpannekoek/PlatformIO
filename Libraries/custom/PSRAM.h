#ifndef PSRAM_H
#define PSRAM_H

#include <Tracer.h>

#ifdef BOARD_HAS_PSRAM
    #define ESP_MALLOC(size) ps_malloc((size))
#else
    #define ESP_MALLOC(size) malloc((size))
#endif

enum struct MemoryType
{
    Auto = 0,
    Internal,
    External
};

constexpr size_t MEMORY_THRESHOLD = 1024;

class Memory
{
    public:
        template<typename T>
        static T* allocate(size_t count, MemoryType memoryType = MemoryType::Auto)
        {
            size_t size = sizeof(T) * count;
            TRACE("Memory::allocate(%u, %d) => %u bytes", count, memoryType, size);

            void* memoryPtr;
#ifdef BOARD_HAS_PSRAM
            uint32_t caps = MALLOC_CAP_8BIT;
            switch (memoryType)
            {
                case MemoryType::Auto:
                    caps |= (size >= MEMORY_THRESHOLD) ? MALLOC_CAP_SPIRAM : MALLOC_CAP_INTERNAL;
                    break;

                case MemoryType::External:
                    caps |= MALLOC_CAP_SPIRAM;
                    break;

                case MemoryType::Internal:
                    caps |= MALLOC_CAP_INTERNAL;
                    break;
            }
            memoryPtr = heap_caps_malloc(size, caps);
            TRACE((caps & MALLOC_CAP_SPIRAM) ? " external" : " internal");            
#else
            memoryPtr = malloc(size);
#endif
            TRACE(" (%p)\n", memoryPtr);
            return static_cast<T*>(memoryPtr);
        }
};

#endif