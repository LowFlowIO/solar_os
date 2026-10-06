#include "solar_os_inputronic_keyboard.h"
#include "solar_os_inputronic_keyboard_profile.h"
#include "solar_os_tca8418.h"

esp_err_t solar_os_inputronic_keyboard_attach(
    const char *name, const solar_os_expansion_binding_t *bindings, size_t count)
{
    solar_os_matrix_keyboard_map_t map;
    solar_os_inputronic_keyboard_map(&map);
    return solar_os_tca8418_attach_profile(name, bindings, count, &map);
}

esp_err_t solar_os_inputronic_keyboard_detach(const char *name)
{
    return solar_os_tca8418_detach(name);
}
