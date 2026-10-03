#if SOLAR_OS_PACKAGE_SERVICE_INFERENCE
#include "solar_os_inference.h"
static solar_os_inference_t *solua_inference_session;
static bool solua_inference_active(void)
{
    return __atomic_load_n(&solua_inference_session, __ATOMIC_ACQUIRE) != NULL;
}
static void solua_inference_destroy(void)
{
    solar_os_inference_destroy(solua_inference_session);
    __atomic_store_n(&solua_inference_session, NULL, __ATOMIC_RELEASE);
}
static solar_os_inference_t *solua_inference_get(lua_State *L)
{
    if (!solua_inference_session) {
        solar_os_inference_t *created = NULL;
        (void)solua_check_esp(L, solar_os_inference_create(solua_should_cancel, NULL, &created));
        __atomic_store_n(&solua_inference_session, created, __ATOMIC_RELEASE);
    }
    return solua_inference_session;
}
static uint32_t solua_inference_uint(lua_State *L, int index)
{
    lua_Integer value = luaL_checkinteger(L, index);
    if (value < 0 || (uint64_t)value > UINT32_MAX) luaL_error(L, "inference integer out of range");
    return value;
}
static void solua_inference_field(lua_State *L, int table, const char *key)
{
    table = lua_absindex(L, table); lua_pushstring(L, key); lua_rawget(L, table);
}
static uint32_t solua_inference_timeout(lua_State *L, int index)
{
    return lua_isnoneornil(L, index) ? 10000U : solua_inference_uint(L, index);
}
static const char *solua_inference_name(lua_State *L, int index)
{
    size_t length;
    const char *name = luaL_checklstring(L, index, &length);
    if (memchr(name, 0, length)) luaL_error(L, "embedded NUL in tensor name/type");
    return name;
}
static void solua_inference_tensor(lua_State *L, const solar_os_inference_tensor_t *t)
{
    lua_newtable(L);
    solua_set_str(L, -1, "dtype", solar_os_tensor_dtype_name(t->dtype));
    solua_set_int(L, -1, "bytes", t->bytes);
    lua_newtable(L);
    for (size_t i = 0; i < t->rank; ++i) { lua_pushinteger(L, t->shape[i]); lua_rawseti(L, -2, i + 1); }
    lua_setfield(L, -2, "shape");
    lua_newtable(L);
    for (size_t i = 0; i < t->exponent_count; ++i) { lua_pushinteger(L, t->exponents[i]); lua_rawseti(L, -2, i + 1); }
    lua_setfield(L, -2, "exponents");
}
static void solua_inference_ports(lua_State *L, const solar_os_inference_tensor_t *ports, size_t count)
{
    lua_newtable(L);
    for (size_t i = 0; i < count; ++i) {
        solua_inference_tensor(L, &ports[i]); lua_setfield(L, -2, ports[i].name);
    }
}
static const solar_os_inference_model_info_t *solua_inference_info(lua_State *L)
{
    const solar_os_inference_model_info_t *info = NULL;
    (void)solua_check_esp(L, solar_os_inference_info(solua_inference_session, solua_inference_uint(L, 1), &info));
    return info;
}
static int solua_inference_load(lua_State *L)
{
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    solua_resolve_path(L, 1, path, sizeof(path));
    uint32_t handle = 0;
    (void)solua_check_esp(L, solar_os_inference_load(solua_inference_get(L), path,
        solua_inference_timeout(L, 2), &handle));
    lua_pushinteger(L, handle); return 1;
}
static int solua_inference_inputs(lua_State *L)
{
    const solar_os_inference_model_info_t *i = solua_inference_info(L);
    solua_inference_ports(L, i->inputs, i->input_count); return 1;
}
static int solua_inference_outputs(lua_State *L)
{
    const solar_os_inference_model_info_t *i = solua_inference_info(L);
    solua_inference_ports(L, i->outputs, i->output_count); return 1;
}
static int solua_inference_info_api(lua_State *L)
{
    const solar_os_inference_model_info_t *i = solua_inference_info(L);
    lua_newtable(L);
    solua_set_str(L, -1, "backend", "espdl"); solua_set_str(L, -1, "target", "esp32s3");
    solua_set_str(L, -1, "backend_version", SOLAR_OS_INFERENCE_BACKEND_VERSION);
    solua_set_str(L, -1, "mode", solar_os_inference_mode_name(i->mode));
    solua_set_int(L, -1, "model_bytes", i->model_bytes);
    solua_set_int(L, -1, "internal_bytes", i->internal_bytes);
    solua_set_int(L, -1, "external_bytes", i->external_bytes);
    solua_inference_ports(L, i->inputs, i->input_count); lua_setfield(L, -2, "inputs");
    solua_inference_ports(L, i->outputs, i->output_count); lua_setfield(L, -2, "outputs");
    return 1;
}
static int solua_inference_result(lua_State *L)
{
    const solar_os_inference_result_t *r = lua_touserdata(L, 1);
    lua_newtable(L);
    solua_set_int(L, -1, "input_us", r->input_us);
    solua_set_int(L, -1, "inference_us", r->inference_us);
    solua_set_int(L, -1, "output_us", r->output_us);
    solua_set_int(L, -1, "elapsed_us", r->elapsed_us);
    lua_newtable(L);
    for (size_t i = 0; i < r->count; ++i) {
        solua_inference_tensor(L, &r->outputs[i].tensor);
        lua_pushlstring(L, r->outputs[i].data, r->outputs[i].tensor.bytes);
        lua_setfield(L, -2, "data"); lua_setfield(L, -2, r->outputs[i].tensor.name);
    }
    lua_setfield(L, -2, "outputs"); return 1;
}
static int solua_inference_run(lua_State *L)
{
    const solar_os_inference_model_info_t *info = solua_inference_info(L);
    uint32_t handle = solua_inference_uint(L, 1), timeout = solua_inference_timeout(L, 3);
    luaL_checktype(L, 2, LUA_TTABLE);
    solar_os_inference_input_t *inputs = lua_newuserdatauv(L, info->input_count * sizeof(*inputs), 0);
    solar_os_inference_tensor_t *typed = lua_newuserdatauv(L, info->input_count * sizeof(*typed), 0);
    memset(inputs, 0, info->input_count * sizeof(*inputs));
    size_t count = 0;
    lua_pushnil(L);
    while (lua_next(L, 2)) {
        if (count == info->input_count || lua_type(L, -2) != LUA_TSTRING)
            return luaL_error(L, "invalid input names/count");
        inputs[count].name = solua_inference_name(L, -2);
        size_t port = 0;
        while (port < info->input_count && strcmp(inputs[count].name, info->inputs[port].name)) ++port;
        if (port == info->input_count) return luaL_error(L, "unknown input name");
        if (lua_istable(L, -1)) {
            int record = lua_absindex(L, -1);
            lua_pushnil(L);
            while (lua_next(L, record)) {
                if (lua_type(L, -2) != LUA_TSTRING) return luaL_error(L, "expected tensor field name");
                const char *key = solua_inference_name(L, -2);
                if (strcmp(key, "data") && strcmp(key, "dtype") && strcmp(key, "shape") &&
                    strcmp(key, "bytes") && strcmp(key, "exponents")) return luaL_error(L, "unknown tensor field");
                lua_pop(L, 1);
            }
            typed[count] = info->inputs[port];
            solua_inference_field(L, record, "dtype");
            (void)solua_check_esp(L, solar_os_tensor_dtype_parse(solua_inference_name(L, -1), &typed[count].dtype));
            lua_pop(L, 1);
            solua_inference_field(L, record, "shape"); luaL_checktype(L, -1, LUA_TTABLE);
            size_t rank = lua_rawlen(L, -1);
            if (rank > SOLAR_OS_INFERENCE_RANK_MAX) return luaL_error(L, "tensor rank too large");
            typed[count].rank = rank;
            for (size_t k = 0; k < rank; ++k) {
                lua_rawgeti(L, -1, k + 1); typed[count].shape[k] = solua_inference_uint(L, -1); lua_pop(L, 1);
            }
            lua_pop(L, 1);
            solua_inference_field(L, record, "exponents");
            if (!lua_isnil(L, -1)) {
                luaL_checktype(L, -1, LUA_TTABLE);
                if (lua_rawlen(L, -1) != typed[count].exponent_count) return luaL_error(L, "quantization mismatch");
                for (size_t k = 0; k < typed[count].exponent_count; ++k) {
                    lua_rawgeti(L, -1, k + 1);
                    if (luaL_checkinteger(L, -1) != typed[count].exponents[k]) return luaL_error(L, "quantization mismatch");
                    lua_pop(L, 1);
                }
            }
            lua_pop(L, 1);
            solua_inference_field(L, record, "data"); luaL_checktype(L, -1, LUA_TSTRING);
            inputs[count].data = lua_tolstring(L, -1, &inputs[count].bytes); lua_pop(L, 1);
            typed[count].bytes = inputs[count].bytes; inputs[count].tensor = &typed[count];
            solua_inference_field(L, record, "bytes");
            if (!lua_isnil(L, -1) && solua_inference_uint(L, -1) != inputs[count].bytes)
                return luaL_error(L, "tensor byte count mismatch");
            lua_pop(L, 1);
        } else {
            luaL_checktype(L, -1, LUA_TSTRING);
            inputs[count].data = lua_tolstring(L, -1, &inputs[count].bytes);
        }
        ++count; lua_pop(L, 1);
    }
    solar_os_inference_result_t *result = NULL;
    (void)solua_check_esp(L, solar_os_inference_run(solua_inference_session, handle, inputs, count, timeout, &result));
    lua_pushcfunction(L, solua_inference_result); lua_pushlightuserdata(L, result);
    int status = lua_pcall(L, 1, 1, 0);
    solar_os_inference_result_free(result);
    return status == LUA_OK ? 1 : lua_error(L);
}
static int solua_inference_reset(lua_State *L)
{
    return solua_check_esp(L, solar_os_inference_reset(solua_inference_session, solua_inference_uint(L, 1)));
}
static int solua_inference_close(lua_State *L)
{
    return solua_check_esp(L, solar_os_inference_close(solua_inference_session, solua_inference_uint(L, 1)));
}
static int solua_inference_set_mode(lua_State *L)
{
    solar_os_inference_mode_t mode;
    (void)solua_check_esp(L, solar_os_inference_mode_parse(solua_inference_name(L, 2), &mode));
    return solua_check_esp(L, solar_os_inference_set_mode(solua_inference_session,
        solua_inference_uint(L, 1), mode));
}
static int solua_inference_close_all(lua_State *L)
{
    (void)L; solua_inference_destroy(); return 0;
}
#include "solar_os_lua_tensor_image.inc"
#else
static bool solua_inference_active(void) { return false; }
static void solua_inference_destroy(void) {}
#endif
