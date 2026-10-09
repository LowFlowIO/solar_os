#pragma once

#include "solar_os_native_service_abi.h"

#define SOLAR_OS_NATIVE_VM_SERVICE "vm"
#define SOLAR_OS_NATIVE_VM_ABI 1U
#define SOLAR_OS_NATIVE_VM_KEY_PRESS 0U
#define SOLAR_OS_NATIVE_VM_KEY_RELEASE 1U
#define SOLAR_OS_NATIVE_VM_KEY_REPEAT 2U

typedef struct {
    uint16_t usage, physical_key;
    uint8_t key, modifiers, action, source;
    bool reset; /* Clear held guest controls before consuming subsequent keys. */
} solar_os_native_vm_key_v1_t;

/* Available only on the foreground VM worker. Calls are serialized on that
 * worker. No host worker calls into the ELF. INDEX8 frames are copied before
 * present returns; the host copies the 256-entry RGB565 palette at video_open.
 * Key values retain SolarOS's logical-key encoding; usage is USB HID when known.
 * Audio is optional, native S16LE interleaved PCM. close on the opening task.
 * Paths must be absolute resolved storage paths. read_file permits short reads;
 * replace_file atomically replaces a complete file after syncing a sibling.
 * Poll stop_requested regularly, including while paused. Release module
 * allocations and return when requested; the host then releases its resources.
 * The host restarts the device if shutdown does not finish within five seconds. */
typedef struct {
    uint32_t abi_version, struct_size;
    uint64_t (*time_us)(void);
    void (*sleep_ms)(uint32_t ms);
    void (*restart)(void);
    int (*video_open)(uint16_t width, uint16_t height,
        const uint16_t *palette_rgb565, uint16_t preferred_fps);
    int (*present)(const uint8_t *pixels, size_t length);
    /* OK with available=false means the queue is empty. */
    int (*read_key)(solar_os_native_vm_key_v1_t *key, bool *available);
    int (*file_size)(const char *path, uint32_t *size);
    int (*read_file)(const char *path, uint32_t offset, void *buffer,
        size_t length, size_t *read_length);
    int (*replace_file)(const char *path, const void *buffer, size_t length);
    int (*audio_open)(uint32_t *sample_rate, uint8_t *channels);
    int (*audio_write)(const int16_t *samples, size_t frames);
    void (*audio_close)(void);
    /* The loader cannot observe C main's return value. Set before returning. */
    void (*set_exit_code)(int code);
    /* Append-only ABI extension. Exit/suspend requests cooperative shutdown. */
    bool (*stop_requested)(void);
} solar_os_native_vm_api_v1_t;
