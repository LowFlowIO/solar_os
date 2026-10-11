#pragma once

#include "solar_os_native_service_abi.h"

/* Application lifecycle 1 is a synchronous command. Lifecycle 2 is a
 * foreground session whose ELF and guest state remain resident until close. */
#define SOLAR_OS_NATIVE_APP_LIFECYCLE_ABI 2U
#define SOLAR_OS_NATIVE_APP_MANIFEST_MAGIC 0x32505041U
#define SOLAR_OS_NATIVE_APP_MANIFEST_SYMBOL "solar_os_native_app_manifest"
#define SOLAR_OS_NATIVE_APP_RESUMABLE (1U << 0)
#define SOLAR_OS_NATIVE_APP_CLASS_TUI 2U
#define SOLAR_OS_NATIVE_APP_CLASS_GUI 3U

/* Fixed-width, pointer-free ELF metadata, checked before executing module
 * code. Memory minima are launch admission requirements, not reservations.
 * Keep internal allocations small; executable code and guest bulk live in
 * PSRAM. Optional caches should tolerate allocation failure. */
typedef struct {
    uint32_t magic, lifecycle_abi, struct_size;
    uint32_t app_class, flags;
    uint32_t worker_stack_bytes, worker_priority;
    uint32_t minimum_internal_bytes, minimum_psram_bytes;
    uint32_t tick_interval_ms;
    char id[32];
    char summary[96];
} solar_os_native_app_manifest_v2_t;

/* Export one const solar_os_native_app_manifest_v2_t with this declaration.
 * Its dynamic symbol lets load distinguish foreground apps from commands
 * independently of filenames and package directories. */
#ifdef __cplusplus
#define SOLAR_OS_NATIVE_APP_LINKAGE extern "C"
#else
#define SOLAR_OS_NATIVE_APP_LINKAGE
#endif
#define SOLAR_OS_NATIVE_APP_MANIFEST SOLAR_OS_NATIVE_APP_LINKAGE \
    __attribute__((used, visibility("default"))) \
    const solar_os_native_app_manifest_v2_t solar_os_native_app_manifest

#define SOLAR_OS_NATIVE_FOREGROUND_SERVICE "foreground"
#define SOLAR_OS_NATIVE_FOREGROUND_ABI 1U
#define SOLAR_OS_NATIVE_FOREGROUND_KEY_PRESS 0U
#define SOLAR_OS_NATIVE_FOREGROUND_KEY_RELEASE 1U
#define SOLAR_OS_NATIVE_FOREGROUND_KEY_REPEAT 2U

typedef struct {
    uint16_t usage, physical_key;
    uint8_t key, modifiers, action, source;
    bool reset;
} solar_os_native_foreground_key_v1_t;

/* Calls belong to the app's worker. Presentation copies submitted pixels;
 * audio closes on its opening task. Poll stop and suspend at guest safe
 * points, including while paused. suspend quiesces host-owned foreground
 * resources and blocks until resume or close, retaining guest state and ELF.
 * After resume, clear held inputs, rebase guest timing, reopen optional audio
 * and submit a full frame. No module callback may survive ELF unload. */
typedef struct {
    uint32_t abi_version, struct_size;
    uint64_t (*time_us)(void);
    void (*sleep_ms)(uint32_t ms);
    void (*restart)(void);
    int (*video_open)(uint16_t width, uint16_t height,
        const uint16_t *palette_rgb565, uint16_t preferred_fps);
    int (*present)(const uint8_t *pixels, size_t length);
    int (*read_key)(solar_os_native_foreground_key_v1_t *key, bool *available);
    int (*file_size)(const char *path, uint32_t *size);
    int (*read_file)(const char *path, uint32_t offset, void *buffer,
        size_t length, size_t *read_length);
    int (*replace_file)(const char *path, const void *buffer, size_t length);
    int (*audio_open)(uint32_t *sample_rate, uint8_t *channels);
    int (*audio_write)(const int16_t *samples, size_t frames);
    void (*audio_close)(void);
    void (*set_exit_code)(int code);
    bool (*stop_requested)(void);
    int (*text_colors)(uint16_t *foreground, uint16_t *background);
    int (*video_open_ex)(uint16_t width, uint16_t height,
        const uint16_t *palette_rgb565, uint16_t preferred_fps,
        uint8_t background_index);
    int (*resolve_path)(const char *path, char *resolved, size_t resolved_size);
    int (*enable_suspend)(void);
    bool (*suspend_requested)(void);
    int (*suspend)(void);
} solar_os_native_foreground_api_v1_t;
