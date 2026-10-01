#pragma once
#include <stdint.h>
#include <stdlib.h>
#define MALLOC_CAP_SPIRAM UINT32_C(1)
#define MALLOC_CAP_8BIT UINT32_C(2)
#define MALLOC_CAP_INTERNAL UINT32_C(4)
extern int jpeg_test_allocations, jpeg_test_fail_alloc;
extern int jpeg_test_fail_psram;
extern size_t jpeg_test_internal_free;
static inline size_t heap_caps_get_free_size(uint32_t caps)
{
    (void)caps;
    return jpeg_test_internal_free;
}
static inline void *heap_caps_aligned_alloc(size_t alignment, size_t size, uint32_t caps)
{
    if (jpeg_test_fail_alloc || (jpeg_test_fail_psram && (caps & MALLOC_CAP_SPIRAM)))
        return NULL;
    void *pointer = aligned_alloc(alignment, (size + alignment - 1) / alignment * alignment);
    if (pointer) jpeg_test_allocations++;
    return pointer;
}
static inline void heap_caps_free(void *pointer)
{
    if (pointer) jpeg_test_allocations--;
    free(pointer);
}
