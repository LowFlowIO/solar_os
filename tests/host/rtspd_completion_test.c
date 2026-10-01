#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "solar_os_shell_rtspd_completion.h"

typedef struct {
    char candidates[16][40];
    size_t count;
} matches_t;

static solar_os_stream_info_t streams[] = {
    {.id = "camera0", .type = SOLAR_OS_STREAM_TYPE_VIDEO,
     .direction = SOLAR_OS_STREAM_DIRECTION_SOURCE, .video = {.codec = SOLAR_OS_STREAM_VIDEO_JPEG}},
    {.id = "network.camera", .type = SOLAR_OS_STREAM_TYPE_VIDEO,
     .direction = SOLAR_OS_STREAM_DIRECTION_SOURCE, .video = {.codec = SOLAR_OS_STREAM_VIDEO_JPEG}},
    {.id = "video-sink", .type = SOLAR_OS_STREAM_TYPE_VIDEO,
     .direction = SOLAR_OS_STREAM_DIRECTION_SINK, .video = {.codec = SOLAR_OS_STREAM_VIDEO_JPEG}},
    {.id = "mic0", .type = SOLAR_OS_STREAM_TYPE_AUDIO,
     .direction = SOLAR_OS_STREAM_DIRECTION_SOURCE,
     .audio = {.sample_format = SOLAR_OS_STREAM_AUDIO_S16_LE, .bits_per_sample = 16, .sample_rate = 16000, .channels = 1}},
    {.id = "audio-duplex", .type = SOLAR_OS_STREAM_TYPE_AUDIO,
     .direction = SOLAR_OS_STREAM_DIRECTION_DUPLEX,
     .audio = {.sample_format = SOLAR_OS_STREAM_AUDIO_S16_LE, .bits_per_sample = 16, .sample_rate = 48000, .channels = 2}},
    {.id = "speaker0", .type = SOLAR_OS_STREAM_TYPE_AUDIO,
     .direction = SOLAR_OS_STREAM_DIRECTION_SINK,
     .audio = {.sample_format = SOLAR_OS_STREAM_AUDIO_S16_LE, .bits_per_sample = 16, .sample_rate = 16000, .channels = 1}},
    {.id = "gpio0", .type = SOLAR_OS_STREAM_TYPE_SCALAR},
    {.id = "bad-audio", .type = SOLAR_OS_STREAM_TYPE_AUDIO,
     .direction = SOLAR_OS_STREAM_DIRECTION_SOURCE,
     .audio = {.bits_per_sample = 8, .sample_rate = 16000, .channels = 1}},
    {.id = "bad-video", .type = SOLAR_OS_STREAM_TYPE_VIDEO,
     .direction = SOLAR_OS_STREAM_DIRECTION_SOURCE, .video = {.codec = (solar_os_stream_video_codec_t)99}},
};

static bool get_stream(size_t index, solar_os_stream_info_t *info, void *context)
{
    (void)context;
    if (index >= sizeof(streams) / sizeof(streams[0])) return false;
    *info = streams[index]; return true;
}

static void emit(const char *candidate, void *context)
{
    matches_t *matches = context;
    assert(matches->count < 16 && strlen(candidate) < 40);
    strcpy(matches->candidates[matches->count++], candidate);
}

static bool contains(const matches_t *m, const char *value)
{
    for (size_t i = 0; i < m->count; i++) if (!strcmp(m->candidates[i], value)) return true;
    return false;
}

static matches_t complete(const char *const *tokens, size_t count, const char *prefix, size_t stream_count)
{
    matches_t matches = {0};
    assert(solar_os_shell_rtspd_completion_emit(tokens, count, prefix,
        stream_count, get_stream, NULL, emit, &matches));
    return matches;
}

int main(void)
{
    const char *const start[] = {"job", "start", "rtspd"};
    const size_t count = sizeof(streams) / sizeof(streams[0]);
    matches_t m = complete(start, 3, NULL, count);
    assert(m.count == 5 && contains(&m, "audio=") && contains(&m, "video="));
    m = complete(start, 3, "a", count);
    assert(m.count == 1 && contains(&m, "audio="));
    m = complete(start, 3, "video=", count + 2); /* Getter failures are harmless. */
    assert(m.count == 3 && contains(&m, "video=none") && contains(&m, "video=camera0"));
    assert(contains(&m, "video=network.camera"));
    assert(!contains(&m, "video=video-sink") && !contains(&m, "video=bad-video"));
    m = complete(start, 3, "audio=", count);
    assert(m.count == 3 && contains(&m, "audio=none") && contains(&m, "audio=mic0"));
    assert(contains(&m, "audio=audio-duplex"));
    assert(!contains(&m, "audio=speaker0") && !contains(&m, "audio=gpio0"));
    m = complete(start, 3, "audio=mi", count);
    assert(m.count == 1 && contains(&m, "audio=mic0"));
    m = complete(start, 3, "video=cam", count);
    assert(m.count == 1 && contains(&m, "video=camera0"));
    m = complete(start, 3, "audio=", 0);
    assert(m.count == 1 && contains(&m, "audio=none"));
    m = complete(start, 3, "size=", 0);
    assert(m.count == 2 && contains(&m, "size=qvga") && contains(&m, "size=vga"));
    m = complete(start, 3, "audio=missing", count);
    assert(m.count == 0);
    const char *const reordered[] = {"job", "start", "rtspd", "port=8554", "video=camera0", "fps=10"};
    m = complete(reordered, 6, "", count);
    assert(m.count == 2 && contains(&m, "audio=") && contains(&m, "size="));
    m = complete(reordered, 6, "audio=", count);
    assert(m.count == 3);
    m = complete(reordered, 6, "video=", count);
    assert(m.count == 0); /* No duplicate options accepted by the job parser. */
    const char *const audio_only[] = {"job", "start", "rtspd", "video=none"};
    m = complete(audio_only, 4, "", count);
    assert(m.count == 2 && contains(&m, "audio=") && contains(&m, "port="));
    const char *const other[] = {"job", "start", "daq"};
    matches_t unmatched = {0};
    assert(!solar_os_shell_rtspd_completion_emit(other, 3, "", count, get_stream, NULL, emit, &unmatched));
    assert(!solar_os_shell_rtspd_completion_emit(start, 2, "", count, get_stream, NULL, emit, &unmatched));
    assert(!solar_os_shell_rtspd_completion_emit(NULL, 3, "", count, get_stream, NULL, emit, &unmatched));
    assert(!solar_os_shell_rtspd_completion_emit(start, 3, "", count, NULL, NULL, emit, &unmatched));
    /* Changes in registration and PCM format are reflected on the next Tab. */
    streams[3].audio.sample_rate = 7999;
    m = complete(start, 3, "audio=mi", count); assert(m.count == 0);
    streams[3].audio.sample_rate = 8000;
    m = complete(start, 3, "audio=mi", count); assert(m.count == 1);
    streams[3].audio.sample_rate = 192001;
    m = complete(start, 3, "audio=mi", count); assert(m.count == 0);
    streams[3].audio.sample_rate = 192000;
    streams[3].audio.channels = 9;
    m = complete(start, 3, "audio=mi", count); assert(m.count == 0);
    streams[3].audio.channels = 8;
    m = complete(start, 3, "audio=mi", count); assert(m.count == 1);
    strcpy(streams[0].id, "attached-camera");
    m = complete(start, 3, "video=attached", count);
    assert(m.count == 1 && contains(&m, "video=attached-camera"));
    puts("rtspd completion tests: OK");
    return 0;
}
