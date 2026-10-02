/* Compile the actual vision binding section, alongside the native service and
 * JPEG/PNG/ownership suite. Only the enclosing interpreter context is stubbed. */
#define main native_vision_suite
#include "vision_test.c"
#undef main
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

static bool fail_lua, fail_result;
static struct { solar_os_raster_image_t *images[1]; } solua;
static size_t solua_image_slot(lua_State *L, int index)
{
    if (luaL_checkinteger(L, index) != 1 || !solua.images[0]) luaL_error(L, "invalid image");
    return 0;
}
static bool solua_should_cancel(void *user) { (void)user; return false; }
static int solua_check_esp(lua_State *L, esp_err_t err)
{
    if (err != ESP_OK) return luaL_error(L, "vision error %d", err);
    if (fail_result) fail_lua = true;
    return 0;
}
static void solua_set_int(lua_State *L, int table, const char *key, lua_Integer value)
{ table = lua_absindex(L, table); lua_pushinteger(L, value); lua_setfield(L, table, key); }
static void solua_set_bool(lua_State *L, int table, const char *key, bool value)
{ table = lua_absindex(L, table); lua_pushboolean(L, value); lua_setfield(L, table, key); }
static uint32_t solua_media_option(lua_State *L, const char *key, uint32_t fallback, uint32_t max)
{
    if (lua_isnoneornil(L, 2)) return fallback;
    lua_getfield(L, 2, key);
    lua_Integer n = lua_isnil(L, -1) ? fallback : luaL_checkinteger(L, -1);
    if (n < 0 || (uint64_t)n > max) luaL_error(L, "option out of range");
    lua_pop(L, 1); return n;
}
#define SOLAR_OS_PACKAGE_SERVICE_VISION 1
#include "solar_os_lua_vision.inc"

static void *lua_alloc(void *user, void *ptr, size_t old, size_t size)
{
    (void)user; (void)old;
    if (!size) { free(ptr); return NULL; }
    if (fail_lua) return NULL;
    return realloc(ptr, size);
}
static void run(lua_State *L, const char *script)
{
    int status = luaL_dostring(L, script);
    if (status != LUA_OK) fprintf(stderr, "Lua: %s\n", lua_tostring(L, -1));
    assert(status == LUA_OK); lua_settop(L, 0);
}
int main(void)
{
    assert(native_vision_suite() == 0);
    solua.images[0] = open_fixture("multiple.png");
    int baseline = allocations;
    lua_State *L = lua_newstate(lua_alloc, NULL); assert(L);
    luaL_requiref(L, "_G", luaopen_base, 1); lua_pop(L, 1);
    luaL_requiref(L, LUA_STRLIBNAME, luaopen_string, 1); lua_pop(L, 1);
    lua_pushcfunction(L, solua_vision_qrcodes); lua_setglobal(L, "qrcodes");
    run(L, "local r = qrcodes(1); assert(#r.codes == 2); "
        "assert(r.width == 416 and r.processed_height == 198 and r.elapsed_us > 0); "
        "assert(r.codes[1].payload == 'SolarOS QR' .. string.char(0) .. 'binary' .. string.char(255)); "
        "assert(#r.codes[1].corners == 4 and #r.codes[1].corners[1] == 2); "
        "r = qrcodes(1,{x=218,width=198,height=198,output_width=99,output_height=99}); "
        "assert(#r.codes == 1 and r.codes[1].corners[1][1] >= 218); "
        "assert(not pcall(qrcodes,99)); assert(not pcall(qrcodes,1,{x=-1})); "
        "assert(not pcall(qrcodes,1,{output_width=641})); "
        "assert(not pcall(qrcodes,1,{output_widht=99})); "
        "assert(not pcall(qrcodes,1,{[2]=99})); "
        "assert(not pcall(qrcodes,1,{x=4294967295})); "
        "assert(not pcall(qrcodes,1,'invalid')); retained = r.codes[1].payload");
    assert(allocations == baseline && !workers);
    lua_pushcfunction(L, solua_vision_qrcodes); lua_pushinteger(L, 1);
    fail_result = true;
    int status = lua_pcall(L, 1, 1, 0);
    fail_result = fail_lua = false;
    assert(status == LUA_ERRMEM && allocations == baseline && !workers);
    lua_settop(L, 0);
    solar_os_raster_image_release(solua.images[0]); solua.images[0] = NULL;
    assert(!allocations);
    run(L, "assert(#retained == 18); assert(not pcall(qrcodes,1))");
    lua_close(L);
    puts("actual Lua QR options/binary/corners/result-OOM cleanup tests passed");
    return 0;
}
