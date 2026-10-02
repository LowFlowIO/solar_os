#pragma once
#include "../esp_heap_caps.h"
#define MALLOC_CAP_DMA UINT32_C(4)
#define MALLOC_CAP_INTERNAL UINT32_C(8)
static inline void *heap_caps_calloc(size_t count, size_t size, uint32_t caps)
{
    (void)caps;
    return calloc(count, size);
}
