#pragma once

#include "solar_os_stream.h"

typedef bool (*solar_os_shell_rtspd_stream_getter_t)(
    size_t index, solar_os_stream_info_t *info, void *context);
typedef void (*solar_os_shell_rtspd_candidate_emitter_t)(
    const char *candidate, void *context);

/* Tokens precede the current argument, after alias expansion. Enumerate only
 * registered sources compatible with rtspd, without opening/claiming them.
 * Return true when this is an rtspd argument position, even with no matches. */
bool solar_os_shell_rtspd_completion_emit(
    const char *const *tokens, size_t token_count, const char *prefix,
    size_t stream_count, solar_os_shell_rtspd_stream_getter_t stream_getter,
    void *stream_context, solar_os_shell_rtspd_candidate_emitter_t emitter,
    void *candidate_context);
