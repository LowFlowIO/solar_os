#include "solar_os_shell_rtspd_completion.h"

#include <stdio.h>
#include <string.h>

static bool begins(const char *text, const char *prefix)
{
    return strncmp(text, prefix, strlen(prefix)) == 0;
}

static void emit_match(const char *candidate, const char *prefix,
    solar_os_shell_rtspd_candidate_emitter_t emitter, void *context)
{
    if (begins(candidate, prefix)) emitter(candidate, context);
}

static bool option_used(const char *const *tokens, size_t count, const char *key)
{
    for (size_t i = 3U; i < count; i++) {
        if (begins(tokens[i], key)) return true;
    }
    return false;
}

static bool audio_source(const solar_os_stream_info_t *info)
{
    /* Match rtspd_audio_format_valid and the job's read-side validation. */
    return info->type == SOLAR_OS_STREAM_TYPE_AUDIO &&
        info->direction != SOLAR_OS_STREAM_DIRECTION_SINK &&
        info->audio.sample_format == SOLAR_OS_STREAM_AUDIO_S16_LE &&
        info->audio.bits_per_sample == 16U && info->audio.channels >= 1U &&
        info->audio.channels <= 8U && info->audio.sample_rate >= 8000U &&
        info->audio.sample_rate <= 192000U;
}

bool solar_os_shell_rtspd_completion_emit(
    const char *const *tokens, size_t token_count, const char *prefix,
    size_t stream_count, solar_os_shell_rtspd_stream_getter_t stream_getter,
    void *stream_context, solar_os_shell_rtspd_candidate_emitter_t emitter,
    void *candidate_context)
{
    if (!tokens || token_count < 3U || !stream_getter || !emitter) return false;
    for (size_t i = 0; i < token_count; i++) if (!tokens[i]) return false;
    if (strcmp(tokens[0], "job") || strcmp(tokens[1], "start") ||
        strcmp(tokens[2], "rtspd")) return false;
    if (!prefix) prefix = "";
    bool video_disabled = false;
    for (size_t i = 3U; i < token_count; i++) {
        if (!strcmp(tokens[i], "video=none")) video_disabled = true;
    }

    const char *const keys[] = {"audio=", "video=", "size=", "fps=", "port="};
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        const char *key = keys[i];
        if (video_disabled && (i == 2U || i == 3U)) continue;
        if (option_used(tokens, token_count, key)) continue;
        if (!begins(prefix, key)) {
            emit_match(key, prefix, emitter, candidate_context);
            continue;
        }
        if (i < 2U) {
            char candidate[sizeof("video=") + SOLAR_OS_STREAM_ID_MAX];
            snprintf(candidate, sizeof(candidate), "%snone", key);
            emit_match(candidate, prefix, emitter, candidate_context);
            for (size_t stream = 0; stream < stream_count; stream++) {
                solar_os_stream_info_t info;
                if (!stream_getter(stream, &info, stream_context)) continue;
                const bool compatible = i == 0U ? audio_source(&info) :
                    info.type == SOLAR_OS_STREAM_TYPE_VIDEO &&
                    info.direction == SOLAR_OS_STREAM_DIRECTION_SOURCE &&
                    info.video.codec == SOLAR_OS_STREAM_VIDEO_JPEG;
                if (!compatible) continue;
                snprintf(candidate, sizeof(candidate), "%s%s", key, info.id);
                emit_match(candidate, prefix, emitter, candidate_context);
            }
        } else if (i == 2U) {
            emit_match("size=qvga", prefix, emitter, candidate_context);
            emit_match("size=vga", prefix, emitter, candidate_context);
        }
    }
    return true;
}
