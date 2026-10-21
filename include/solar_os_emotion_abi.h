#pragma once

#include <stddef.h>
#include <stdint.h>
#include "solar_os_native_service_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SOLAR_OS_EMOTION_LIBRARY "emotion"
#define SOLAR_OS_EMOTION_ABI 1U

enum {
    SOLAR_OS_EMOTION_NEUTRAL, SOLAR_OS_EMOTION_BLINK_HIGH,
    SOLAR_OS_EMOTION_HAPPY, SOLAR_OS_EMOTION_GLEE,
    SOLAR_OS_EMOTION_BLINK_LOW, SOLAR_OS_EMOTION_SAD_DOWN,
    SOLAR_OS_EMOTION_SAD_UP, SOLAR_OS_EMOTION_WORRIED,
    SOLAR_OS_EMOTION_FOCUSED, SOLAR_OS_EMOTION_ANNOYED,
    SOLAR_OS_EMOTION_SURPRISED, SOLAR_OS_EMOTION_SKEPTIC,
    SOLAR_OS_EMOTION_BORED, SOLAR_OS_EMOTION_UNIMPRESSED,
    SOLAR_OS_EMOTION_SLEEPY, SOLAR_OS_EMOTION_SUSPICIOUS,
    SOLAR_OS_EMOTION_SQUINT, SOLAR_OS_EMOTION_ANGRY,
    SOLAR_OS_EMOTION_FURIOUS, SOLAR_OS_EMOTION_SCARED,
    SOLAR_OS_EMOTION_AWE, SOLAR_OS_EMOTION_POSE_COUNT
};
enum {
    SOLAR_OS_EMOTION_SILENT, SOLAR_OS_EMOTION_CHIRP,
    SOLAR_OS_EMOTION_GIGGLE, SOLAR_OS_EMOTION_SIGH,
    SOLAR_OS_EMOTION_ALERT, SOLAR_OS_EMOTION_CUE_COUNT
};

/* Caller owns state and pixels. Serialize calls on each state. Time is
 * monotonic milliseconds; the host may pause its clock during suspension.
 * State requires ordinary malloc alignment. No OS resources or callbacks
 * are retained by the engine. All methods use native service result codes. */
typedef struct {
    uint32_t abi_version, struct_size;
    size_t state_bytes;
    const char *(*pose_name)(uint32_t pose);
    int (*pose_find)(const char *name);
    int (*init)(void *state, uint64_t now_ms, uint32_t seed);
    int (*set)(void *state, uint32_t pose, uint32_t transition_ms, uint64_t now_ms);
    int (*react)(void *state, uint32_t pose, uint32_t hold_ms,
                 uint32_t transition_ms, uint64_t now_ms);
    /* x/y are -1000..1000; negative y looks up. */
    int (*look)(void *state, int32_t x, int32_t y);
    int (*blink)(void *state, uint64_t now_ms);
    /* INDEX2, LSB-first, background=0, eyes=3. Clears row padding too.
     * animate=0 suppresses automatic blinking/idle motion for slow panels. */
    int (*render)(void *state, uint64_t now_ms, uint8_t *pixels, size_t bytes,
                  uint16_t width, uint16_t height, uint16_t stride, int animate);
    uint32_t (*cue_duration_ms)(uint32_t cue);
    /* Stateless mono S16 PCM, at most 4096 samples per call. Offset and
     * rate identify a continuous cue; samples beyond its end are zero. */
    int (*sound)(uint32_t cue, uint32_t rate, uint32_t offset,
                 int16_t *samples, size_t count);
} solar_os_emotion_api_v1_t;

#ifdef __cplusplus
}
#endif
