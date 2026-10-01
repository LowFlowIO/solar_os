/* Mirrored media bindings; native services own leases and workers. */
static solar_os_script_media_t *solua_media_session;

static solar_os_script_media_t *solua_media_get(lua_State *L)
{
    if (!solua_media_session) {
        solar_os_script_media_t *created = NULL;
        (void)solua_check_esp(L, solar_os_script_media_create(
            solua_runner_control ? "lua.runner" : "lua.app",
            solua_should_cancel, NULL, &created));
        __atomic_store_n(&solua_media_session, created, __ATOMIC_RELEASE);
    }
    return solua_media_session;
}

static void solua_media_destroy(void)
{
    solar_os_script_media_destroy(solua_media_session);
    __atomic_store_n(&solua_media_session, NULL, __ATOMIC_RELEASE);
}

static uint32_t solua_media_uint(lua_State *L, int index, uint32_t max)
{
    lua_Integer value = luaL_checkinteger(L, index);
    if (value < 0 || (uint64_t)value > max) luaL_error(L, "media value out of range");
    return (uint32_t)value;
}

static uint32_t solua_media_option(lua_State *L, const char *key, uint32_t fallback, uint32_t max)
{
    if (lua_isnoneornil(L, 2)) return fallback;
    luaL_checktype(L, 2, LUA_TTABLE);
    lua_getfield(L, 2, key);
    uint32_t value = lua_isnil(L, -1) ? fallback : solua_media_uint(L, -1, max);
    lua_pop(L, 1);
    return value;
}

static void solua_stream_info(lua_State *L, const solar_os_stream_info_t *info)
{
    lua_newtable(L);
    solua_set_str(L, -1, "id", info->id);
    solua_set_str(L, -1, "provider", info->provider);
    solua_set_str(L, -1, "device", info->device);
    solua_set_str(L, -1, "unit", info->unit);
    solua_set_str(L, -1, "format", info->format);
    solua_set_str(L, -1, "summary", info->summary);
    solua_set_str(L, -1, "owner", info->owner);
    solua_set_str(L, -1, "type", solar_os_stream_type_name(info->type));
    solua_set_str(L, -1, "direction", solar_os_stream_direction_name(info->direction));
    solua_set_str(L, -1, "sharing", solar_os_stream_sharing_name(info->sharing));
    solua_set_int(L, -1, "active_handles", info->active_handles);
    solua_set_int(L, -1, "read_units", info->read_units);
    solua_set_int(L, -1, "written_units", info->written_units);
    solua_set_int(L, -1, "overruns", info->overruns);
    solua_set_int(L, -1, "underruns", info->underruns);
    if (info->type == SOLAR_OS_STREAM_TYPE_VIDEO) {
        solua_set_str(L, -1, "codec", "jpeg");
        solua_set_int(L, -1, "width", info->video.width);
        solua_set_int(L, -1, "height", info->video.height);
        solua_set_int(L, -1, "jpeg_quality", info->video.jpeg_quality);
    } else if (info->type == SOLAR_OS_STREAM_TYPE_AUDIO) {
        solua_set_str(L, -1, "sample_format", solar_os_stream_audio_sample_format_name(info->audio.sample_format));
        solua_set_int(L, -1, "sample_rate", info->audio.sample_rate);
        solua_set_int(L, -1, "channels", info->audio.channels);
        solua_set_int(L, -1, "bits_per_sample", info->audio.bits_per_sample);
        solua_set_int(L, -1, "frames_per_block", info->audio.frames_per_block);
    }
}

static int solua_streams_list(lua_State *L)
{
    lua_newtable(L);
    const size_t count = solar_os_stream_count();
    lua_Integer index = 1;
    for (size_t i = 0; i < count; ++i) {
        solar_os_stream_info_t info;
        if (solar_os_stream_get(i, &info)) {
            solua_stream_info(L, &info);
            lua_rawseti(L, -2, index++);
        }
    }
    return 1;
}

static int solua_streams_info(lua_State *L)
{
    solar_os_stream_info_t info;
    (void)solua_check_esp(L, solar_os_stream_get_info(luaL_checkstring(L, 1), &info));
    solua_stream_info(L, &info);
    return 1;
}

static int solua_streams_status(lua_State *L)
{
    solar_os_stream_info_t info;
    (void)solua_check_esp(L, solar_os_script_media_stream_info(solua_media_session, solua_media_uint(L, 1, UINT32_MAX), &info));
    solua_stream_info(L, &info);
    return 1;
}

static int solua_streams_open(lua_State *L)
{
    const char *id = luaL_checkstring(L, 1);
    solar_os_stream_info_t info;
    (void)solua_check_esp(L, solar_os_stream_get_info(id, &info));
    if (!lua_isnoneornil(L, 2)) {
        luaL_checktype(L, 2, LUA_TTABLE);
        lua_pushnil(L);
        while (lua_next(L, 2)) {
            if (lua_type(L, -2) != LUA_TSTRING) return luaL_error(L, "expected string option key");
            const char *key = lua_tostring(L, -2);
            if (!solar_os_script_media_option_valid(info.type, key))
                return luaL_error(L, "unsupported stream option %s", key);
            lua_pop(L, 1);
        }
    }
    solar_os_stream_open_options_t options = {
        .direction = info.direction == SOLAR_OS_STREAM_DIRECTION_SINK ?
            SOLAR_OS_STREAM_DIRECTION_SINK : SOLAR_OS_STREAM_DIRECTION_SOURCE,
        .timeout_ms = solua_media_option(L, "timeout_ms", 1000, SOLAR_OS_SCRIPT_MEDIA_TIMEOUT_MAX),
    };
    if (!lua_isnoneornil(L, 2)) {
        lua_getfield(L, 2, "direction");
        if (!lua_isnil(L, -1)) {
            const char *name = luaL_checkstring(L, -1);
            if (!strcmp(name, "source")) options.direction = SOLAR_OS_STREAM_DIRECTION_SOURCE;
            else if (!strcmp(name, "sink")) options.direction = SOLAR_OS_STREAM_DIRECTION_SINK;
            else if (!strcmp(name, "duplex")) options.direction = SOLAR_OS_STREAM_DIRECTION_DUPLEX;
            else return luaL_error(L, "expected source, sink or duplex");
        }
        lua_pop(L, 1);
    }
    if (info.type == SOLAR_OS_STREAM_TYPE_VIDEO) {
        options.requested_video = (solar_os_stream_video_format_t){
            .codec = SOLAR_OS_STREAM_VIDEO_JPEG,
            .width = solua_media_option(L, "width", 0, UINT16_MAX),
            .height = solua_media_option(L, "height", 0, UINT16_MAX),
            .jpeg_quality = solua_media_option(L, "jpeg_quality", 0, 63),
        };
    } else if (info.type == SOLAR_OS_STREAM_TYPE_AUDIO) {
        options.requested_audio = info.audio;
        options.requested_audio.sample_rate = solua_media_option(L, "sample_rate", info.audio.sample_rate, 192000);
        options.requested_audio.channels = solua_media_option(L, "channels", info.audio.channels, 8);
        options.requested_audio.frames_per_block = solua_media_option(L, "frames_per_block", info.audio.frames_per_block, 4096);
    }
    uint32_t handle;
    (void)solua_check_esp(L, solar_os_script_media_open(solua_media_get(L), id, &options, &handle));
    lua_pushinteger(L, handle);
    return 1;
}

static int solua_streams_close(lua_State *L)
{
    return solua_check_esp(L, solar_os_script_media_close(solua_media_session, solua_media_uint(L, 1, UINT32_MAX)));
}

static int solua_streams_close_all(lua_State *L)
{
    return solua_check_esp(L, solar_os_script_media_close_all(solua_media_session));
}

static int solua_streams_read(lua_State *L)
{
    uint32_t handle = solua_media_uint(L, 1, UINT32_MAX);
    size_t size = solua_media_uint(L, 2, SOLAR_OS_SCRIPT_MEDIA_TRANSFER_MAX);
    uint32_t timeout = lua_isnoneornil(L, 3) ? 1000 : solua_media_uint(L, 3, SOLAR_OS_SCRIPT_MEDIA_TIMEOUT_MAX);
    if (!size) return luaL_error(L, "read size must be 1..16384");
    /* Lua's buffer is GC-owned if a conversion raises; no native scratch leak. */
    luaL_Buffer buffer;
    char *data = luaL_buffinitsize(L, &buffer, size);
    size_t length;
    esp_err_t err = solar_os_script_media_read(solua_media_session, handle, data, size, timeout, &length);
    (void)solua_check_esp(L, err);
    luaL_pushresultsize(&buffer, length);
    return 1;
}

static int solua_streams_write(lua_State *L)
{
    uint32_t handle = solua_media_uint(L, 1, UINT32_MAX);
    size_t size, length;
    const char *data = luaL_checklstring(L, 2, &size);
    uint32_t timeout = lua_isnoneornil(L, 3) ? 1000 : solua_media_uint(L, 3, SOLAR_OS_SCRIPT_MEDIA_TIMEOUT_MAX);
    (void)solua_check_esp(L, solar_os_script_media_write(solua_media_session, handle, data, size, timeout, &length));
    lua_pushinteger(L, length);
    return 1;
}

static int solua_streams_read_scalar(lua_State *L)
{
    const solar_os_stream_read_options_t options = {
        .window_ms = lua_isnoneornil(L, 2) ? 0 : solua_media_uint(L, 2, SOLAR_OS_SCRIPT_MEDIA_TIMEOUT_MAX),
        .timeout_ms = lua_isnoneornil(L, 3) ? 1000 : solua_media_uint(L, 3, SOLAR_OS_SCRIPT_MEDIA_TIMEOUT_MAX),
    };
    float value;
    (void)solua_check_esp(L, solar_os_script_media_scalar(solua_media_session, solua_media_uint(L, 1, UINT32_MAX), &options, &value));
    lua_pushnumber(L, value);
    return 1;
}

static int solua_streams_acquire_frame(lua_State *L)
{
    uint32_t frame;
    (void)solua_check_esp(L, solar_os_script_media_acquire(solua_media_session, solua_media_uint(L, 1, UINT32_MAX), &frame));
    lua_pushinteger(L, frame);
    return 1;
}

static int solua_streams_frame_info(lua_State *L)
{
    solar_os_script_media_frame_t frame;
    (void)solua_check_esp(L, solar_os_script_media_frame(solua_media_session, solua_media_uint(L, 1, UINT32_MAX), &frame));
    lua_newtable(L);
    solua_set_str(L, -1, "codec", "jpeg");
    solua_set_int(L, -1, "width", frame.jpeg.width);
    solua_set_int(L, -1, "height", frame.jpeg.height);
    solua_set_int(L, -1, "length", frame.jpeg.length);
    solua_set_int(L, -1, "timestamp_us", frame.jpeg.timestamp_us);
    solua_set_bool(L, -1, "network", frame.network);
    if (frame.network) {
        solua_set_int(L, -1, "rtp_timestamp", frame.rtp_timestamp);
        solua_set_int(L, -1, "arrived_us", frame.arrived_us);
    }
    return 1;
}

static int solua_streams_frame_data(lua_State *L)
{
    solar_os_script_media_frame_t frame;
    (void)solua_check_esp(L, solar_os_script_media_frame(solua_media_session, solua_media_uint(L, 1, UINT32_MAX), &frame));
    lua_pushlstring(L, (const char *)frame.jpeg.data, frame.jpeg.length);
    return 1;
}

static int solua_streams_frame_save(lua_State *L)
{
    uint32_t frame = solua_media_uint(L, 1, UINT32_MAX);
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    solua_resolve_path(L, 2, path, sizeof(path));
    return solua_check_esp(L, solar_os_script_media_save(solua_media_session, frame, path));
}

static int solua_streams_release_frame(lua_State *L)
{
    return solua_check_esp(L, solar_os_script_media_release(solua_media_session, solua_media_uint(L, 1, UINT32_MAX)));
}

#if SOLAR_OS_PACKAGE_SERVICE_CAMERA
static int solua_camera_status(lua_State *L)
{
    solar_os_camera_status_t status;
    (void)solua_check_esp(L, solar_os_camera_get_status(&status));
    lua_newtable(L);
    solua_set_bool(L, -1, "backend_registered", status.backend_registered);
    solua_set_bool(L, -1, "initialized", status.initialized);
    solua_set_bool(L, -1, "owner_leased", status.owner_leased);
    solua_set_bool(L, -1, "frame_leased", status.frame_leased);
    solua_set_str(L, -1, "owner", status.owner);
    solua_set_str(L, -1, "driver", status.driver);
    solua_set_str(L, -1, "sensor", status.sensor.name);
    solua_set_str(L, -1, "size", solar_os_camera_frame_size_name(status.config.frame_size));
    solua_set_int(L, -1, "jpeg_quality", status.config.jpeg_quality);
    solua_set_int(L, -1, "capture_count", status.capture_count);
    solua_set_str(L, -1, "last_error_name", esp_err_to_name(status.last_error));
    return 1;
}

static uint32_t solua_camera_snapshot_handle(lua_State *L, int start)
{
    const char *size = luaL_optstring(L, start, "qvga");
    bool vga = !strcmp(size, "vga");
    if (!vga && strcmp(size, "qvga")) luaL_error(L, "expected qvga or vga");
    uint32_t quality = lua_isnoneornil(L, start + 1) ? 12 : solua_media_uint(L, start + 1, 63);
    const char *source = luaL_optstring(L, start + 2, "camera0");
    uint32_t frame;
    (void)solua_check_esp(L, solar_os_script_media_snapshot(solua_media_get(L), source,
        vga ? 640 : 320, vga ? 480 : 240, quality, &frame));
    return frame;
}

static int solua_camera_snapshot(lua_State *L)
{
    lua_pushinteger(L, solua_camera_snapshot_handle(L, 1));
    return 1;
}

static int solua_camera_capture(lua_State *L)
{
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    solua_resolve_path(L, 1, path, sizeof(path));
    uint32_t frame = solua_camera_snapshot_handle(L, 2);
    esp_err_t err = solar_os_script_media_save(solua_media_session, frame, path);
    esp_err_t release = solar_os_script_media_release(solua_media_session, frame);
    return solua_check_esp(L, err != ESP_OK ? err : release);
}
#endif

#if SOLAR_OS_PACKAGE_SERVICE_RTSP_CLIENT
static int solua_rtsp_open(lua_State *L)
{
    const char *url = luaL_checkstring(L, 1);
    bool video = lua_isnoneornil(L, 2) ? true : lua_toboolean(L, 2);
    bool audio = lua_isnoneornil(L, 3) ? true : lua_toboolean(L, 3);
    uint32_t handle;
    (void)solua_check_esp(L, solar_os_script_media_rtsp_open(solua_media_get(L), url, video, audio, &handle));
    lua_pushinteger(L, handle);
    return 1;
}

static int solua_rtsp_status(lua_State *L)
{
    solar_os_rtsp_client_status_t status;
    bool ended;
    (void)solua_check_esp(L, solar_os_script_media_rtsp_status(solua_media_session,
        solua_media_uint(L, 1, UINT32_MAX), &status, &ended));
    lua_newtable(L);
    solua_set_bool(L, -1, "ended", ended);
    solua_set_bool(L, -1, "negotiated", status.negotiated);
    solua_set_bool(L, -1, "playing", status.playing);
    solua_set_bool(L, -1, "audio_playing", status.audio_playing);
    solua_set_bool(L, -1, "video", status.video);
    solua_set_bool(L, -1, "audio", status.audio);
    solua_set_int(L, -1, "sample_rate", status.sample_rate);
    solua_set_int(L, -1, "channels", status.channels);
    solua_set_int(L, -1, "video_frames", status.video_frames);
    solua_set_int(L, -1, "video_dropped", status.video_dropped);
    solua_set_int(L, -1, "audio_dropped", status.audio_dropped);
    solua_set_int(L, -1, "error", status.error);
    solua_set_str(L, -1, "error_name", esp_err_to_name(status.error));
    solua_set_str(L, -1, "error_detail", status.error_detail);
    return 1;
}

static int solua_rtsp_read_frame(lua_State *L)
{
    uint32_t frame;
    (void)solua_check_esp(L, solar_os_script_media_rtsp_read(solua_media_session,
        solua_media_uint(L, 1, UINT32_MAX),
        lua_isnoneornil(L, 2) ? 0 : solua_media_uint(L, 2, SOLAR_OS_SCRIPT_MEDIA_TIMEOUT_MAX), &frame));
    if (frame) lua_pushinteger(L, frame); else lua_pushnil(L);
    return 1;
}

static int solua_rtsp_close(lua_State *L)
{
    solar_os_rtsp_client_status_t status;
    bool ended;
    (void)solua_check_esp(L, solar_os_script_media_rtsp_status(solua_media_session,
        solua_media_uint(L, 1, UINT32_MAX), &status, &ended));
    return solua_streams_close(L);
}

static int solua_rtsp_lateness(lua_State *L)
{
    int64_t lateness;
    (void)solua_check_esp(L, solar_os_script_media_rtsp_lateness(solua_media_session,
        solua_media_uint(L, 1, UINT32_MAX), solua_media_uint(L, 2, UINT32_MAX), &lateness));
    lua_pushinteger(L, lateness);
    return 1;
}
#endif

#if SOLAR_OS_PACKAGE_SERVICE_IMAGE
static size_t solua_image_free_slot(lua_State *L)
{
    for (size_t i = 0; i < SOLUA_RASTER_IMAGE_MAX; ++i) if (!solua.images[i]) return i;
    luaL_error(L, "too many open images");
    return 0;
}

static int solua_image_decode(lua_State *L)
{
    const size_t slot = solua_image_free_slot(L);
    size_t length;
    const char *data = luaL_checklstring(L, 1, &length);
    (void)solua_check_esp(L, solar_os_raster_image_decode((const uint8_t *)data, length, &solua.images[slot]));
    lua_pushinteger(L, slot + 1);
    return 1;
}

static int solua_image_from_frame(lua_State *L)
{
    const size_t slot = solua_image_free_slot(L);
    solar_os_script_media_frame_t frame;
    (void)solua_check_esp(L, solar_os_script_media_frame(solua_media_session, solua_media_uint(L, 1, UINT32_MAX), &frame));
    (void)solua_check_esp(L, solar_os_raster_image_decode(frame.jpeg.data, frame.jpeg.length, &solua.images[slot]));
    lua_pushinteger(L, slot + 1);
    return 1;
}

static int solua_image_present(lua_State *L)
{
    const int count = lua_gettop(L);
    if (count != 3 && count != 5) return luaL_error(L, "expected handle, x, y [, width, height]");
    solar_os_raster_image_t *image = solua.images[solua_image_slot(L, 1)];
    const solua_event_t event = {
        .type = SOLUA_EVENT_IMAGE_DRAW, .attr = 1,
        .x0 = solua_media_uint(L, 2, INT32_MAX), .y0 = solua_media_uint(L, 3, INT32_MAX),
        .width = count == 5 ? solua_check_u16_size(L, 4) : 0,
        .height = count == 5 ? solua_check_u16_size(L, 5) : 0,
        .object = (uintptr_t)image,
    };
    if (event.x0 < 0 || event.y0 < 0 || (count == 5 && (!event.width || !event.height)))
        return luaL_error(L, "invalid presentation rectangle");
    solar_os_raster_image_retain(image);
    if (!solua_send_event(&event)) {
        solar_os_raster_image_release(image);
        return luaL_error(L, "ui event queue stopped");
    }
    return 0;
}
#endif
