#include "solar_os_tca8418.h"

#include <stdlib.h>
#include <string.h>
#include "solar_os_memory.h"
#include "solar_os_matrix_keyboard_json.h"
#include "solar_os_storage.h"

#define KEYMAP_FILE_MAX 16384U

esp_err_t solar_os_tca8418_load_keymap(const char *name, const char *path)
{
    if (name == NULL || path == NULL || path[0] != '/') return ESP_ERR_INVALID_ARG;
    char *json = solar_os_memory_alloc(KEYMAP_FILE_MAX + 2U,
        SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "keyboard-map");
    solar_os_matrix_keyboard_map_t *map = solar_os_memory_alloc(sizeof(*map),
        SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "keyboard-map");
    esp_err_t err = json == NULL || map == NULL ? ESP_ERR_NO_MEM :
        solar_os_tca8418_get_keymap(name, true, map);
    size_t length = 0U;
    if (err == ESP_OK)
        err = solar_os_storage_read_file(path, json, KEYMAP_FILE_MAX + 1U, &length);
    if (err == ESP_OK && length > KEYMAP_FILE_MAX) err = ESP_ERR_INVALID_SIZE;
    if (err == ESP_OK && memchr(json, '\0', length) != NULL) err = ESP_ERR_INVALID_ARG;
    if (err == ESP_OK) {
        json[length] = '\0';
        err = solar_os_matrix_keyboard_parse_map(json, map, map);
    }
    if (err == ESP_OK) err = solar_os_tca8418_set_keymap(name, map);
    free(map);
    free(json);
    return err;
}
