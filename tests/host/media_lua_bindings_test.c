/* Exercise the actual binding conversions with the threaded fake receiver and
 * real local stream/camera services used by the native lifecycle suite. */
#define main native_media_suite
#include "script_media_test.c"
#undef main
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#define SOLAR_OS_STORAGE_PATH_MAX 256U
static void *solua_runner_control;
static bool solua_should_cancel(void *user) { (void)user; return cancel_requested; }
static int solua_check_esp(lua_State *L, esp_err_t err)
{ return err == ESP_OK ? 0 : luaL_error(L, "media error %d", err); }
static void solua_set_str(lua_State *L, int table, const char *key, const char *value)
{ table = lua_absindex(L, table); lua_pushstring(L, value); lua_setfield(L, table, key); }
static void solua_set_int(lua_State *L, int table, const char *key, lua_Integer value)
{ table = lua_absindex(L, table); lua_pushinteger(L, value); lua_setfield(L, table, key); }
static void solua_set_bool(lua_State *L, int table, const char *key, bool value)
{ table = lua_absindex(L, table); lua_pushboolean(L, value); lua_setfield(L, table, key); }
static void solua_resolve_path(lua_State *L, int index, char *path, size_t length)
{ if (strlcpy(path, luaL_checkstring(L, index), length) >= length) luaL_error(L, "path too long"); }
#include "solar_os_lua_media.inc"

static void run(lua_State *L, const char *script)
{
    if (luaL_dostring(L, script) != LUA_OK) {
        fprintf(stderr, "Lua: %s\n", lua_tostring(L, -1)); assert(false);
    }
    lua_settop(L, 0);
}

int main(void)
{
    assert(native_media_suite() == 0);
    const solar_os_camera_backend_ops_t ops = {.start = start, .stop = stop, .capture = capture, .release = release};
    const solar_os_camera_backend_t backend = {.driver = "fake", .ops = &ops};
    assert(solar_os_camera_register_backend(&backend) == ESP_OK);
    assert(solar_os_camera_stream_register("camera0") == ESP_OK);
    lua_State *L = luaL_newstate(); assert(L);
    luaL_requiref(L, "_G", luaopen_base, 1); lua_pop(L, 1);
    luaL_requiref(L, LUA_STRLIBNAME, luaopen_string, 1); lua_pop(L, 1);
    const luaL_Reg stream_methods[] = {
        {"list", solua_streams_list}, {"info", solua_streams_info}, {"status", solua_streams_status},
        {"open", solua_streams_open}, {"close", solua_streams_close}, {"close_all", solua_streams_close_all},
        {"read", solua_streams_read}, {"write", solua_streams_write}, {"read_scalar", solua_streams_read_scalar},
        {"acquire_frame", solua_streams_acquire_frame}, {"frame_info", solua_streams_frame_info},
        {"frame_data", solua_streams_frame_data}, {"frame_save", solua_streams_frame_save},
        {"release_frame", solua_streams_release_frame}, {NULL, NULL},
    };
    const luaL_Reg camera_methods[] = {
        {"status", solua_camera_status}, {"snapshot", solua_camera_snapshot},
        {"capture", solua_camera_capture}, {NULL, NULL},
    };
    const luaL_Reg rtsp_methods[] = {
        {"open", solua_rtsp_open}, {"status", solua_rtsp_status}, {"read_frame", solua_rtsp_read_frame},
        {"lateness", solua_rtsp_lateness}, {"close", solua_rtsp_close}, {NULL, NULL},
    };
    lua_newtable(L); luaL_setfuncs(L, stream_methods, 0); lua_setglobal(L, "streams");
    lua_newtable(L); luaL_setfuncs(L, camera_methods, 0); lua_setglobal(L, "camera");
    lua_newtable(L); luaL_setfuncs(L, rtsp_methods, 0); lua_setglobal(L, "rtsp");
    run(L, "assert(#streams.list() == 3); assert(streams.info('camera0').codec == 'jpeg'); "
        "local h = streams.open('sensor0'); assert(streams.read_scalar(h) == 3.25); streams.close(h); "
        "assert(not pcall(streams.read_scalar, h)); "
        "assert(not pcall(streams.open, 'camera0', {widht=320})); "
        "assert(not pcall(streams.open, 'camera0', {width=-1})); "
        "h = streams.open('bytes0', {direction='duplex'}); "
        "assert(streams.read(h, 3, 0) == 'xxx'); assert(streams.write(h, 'abc', 0) == 3); "
        "assert(not pcall(streams.read, h, 16385)); streams.close(h); "
        "local f = camera.snapshot('vga', 20); local i = streams.frame_info(f); "
        "assert(i.timestamp_us == 123456789012 and i.length == 4); "
        "assert(#streams.frame_data(f) == 4); assert(camera.status().owner_leased); "
        "assert(not pcall(camera.snapshot)); streams.release_frame(f); "
        "assert(not camera.status().owner_leased); assert(not pcall(streams.frame_data, f)); "
        "h = streams.open('camera0', {width=640,height=480,jpeg_quality=20}); "
        "assert(streams.status(h).width == 640); f = streams.acquire_frame(h); "
        "streams.close(h); assert(not pcall(streams.frame_info, f)); "
        "assert(not pcall(camera.capture, '/nonexistent-parent/test.jpg')); "
        "assert(not camera.status().owner_leased); "
        "r = rtsp.open('rtsp://test/media', true, false); assert(rtsp.status(r).video); "
        "assert(rtsp.read_frame(r, 0) == nil)");
    atomic_store(&client->pending, true);
    run(L, "f = rtsp.read_frame(r, 0); assert(streams.frame_info(f).rtp_timestamp == 0xf1234567); "
        "assert(rtsp.lateness(r, f) == -25000); streams.release_frame(f); rtsp.close(r); "
        "assert(not pcall(rtsp.status, r)); "
        "r = rtsp.open('rtsp://test/media', false, true); assert(rtsp.status(r).audio); "
        "f = camera.snapshot(); error_keeps_lease = not pcall(function() error('user failure') end)");
    assert(workers == 1 && clients == 1 && allocations == 1);
    solua_media_destroy(); lua_close(L);
    assert(!workers && !clients && !allocations);
    assert(solar_os_camera_stream_unregister("camera0") == ESP_OK);
    assert(solar_os_camera_unregister_backend("fake") == ESP_OK);
    puts("actual Lua media bindings/conversion/cleanup tests passed");
    return 0;
}
