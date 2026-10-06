#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "solar_os_tca8418.h"
#include "solar_os_memory.h"

static solar_os_matrix_keyboard_map_t active, defaults;
static const char *contents;
static size_t content_length;
static unsigned applies, allocations, fail_allocation;
static esp_err_t device_error, read_error;

void *solar_os_memory_alloc(size_t size, solar_os_memory_class_t memory_class, const char *tag)
{
    assert(memory_class == SOLAR_OS_MEMORY_EXTERNAL_PREFERRED && tag != NULL);
    allocations++;
    return allocations == fail_allocation ? NULL : malloc(size);
}

esp_err_t solar_os_storage_read_file(const char *path, void *buffer,
    size_t buffer_len, size_t *read_len)
{
    assert(strcmp(path, "/map.json") == 0);
    if (read_error != ESP_OK) return read_error;
    *read_len = content_length < buffer_len ? content_length : buffer_len;
    memcpy(buffer, contents, *read_len);
    return ESP_OK;
}

esp_err_t solar_os_tca8418_get_keymap(const char *name, bool built_in,
    solar_os_matrix_keyboard_map_t *map)
{
    assert(strcmp(name, "keyboard0") == 0 && built_in);
    if (device_error != ESP_OK) return device_error;
    *map = defaults;
    return ESP_OK;
}

esp_err_t solar_os_tca8418_set_keymap(const char *name,
    const solar_os_matrix_keyboard_map_t *map)
{
    assert(strcmp(name, "keyboard0") == 0);
    assert(solar_os_matrix_keyboard_validate_map(map) == ESP_OK);
    active = *map;
    applies++;
    return ESP_OK;
}

static void file(const char *text)
{
    contents = text;
    content_length = strlen(text);
}

int main(void)
{
    assert(solar_os_matrix_keyboard_default_map(&defaults, 4, 10) == ESP_OK);
    active = defaults;
    assert(solar_os_tca8418_load_keymap("keyboard0", "relative.json") == ESP_ERR_INVALID_ARG);
    file("{\"schema\":1,\"keys\":[{\"row\":0,\"col\":0,\"usage\":4}]}");
    assert(solar_os_tca8418_load_keymap("keyboard0", "/map.json") == ESP_OK);
    assert(applies == 1 && active.keys[0][1].usage == 4);
    file("{\"schema\":1,\"keys\":[]}");
    assert(solar_os_tca8418_load_keymap("keyboard0", "/map.json") == ESP_OK);
    assert(applies == 2 && active.keys[0][1].usage == defaults.keys[0][1].usage);
    for (unsigned next = 1; next <= 2; next++) {
        fail_allocation = allocations + next;
        assert(solar_os_tca8418_load_keymap("keyboard0", "/map.json") == ESP_ERR_NO_MEM);
        assert(applies == 2);
    }
    fail_allocation = 0;
    device_error = ESP_ERR_NOT_FOUND;
    assert(solar_os_tca8418_load_keymap("keyboard0", "/map.json") == ESP_ERR_NOT_FOUND);
    device_error = ESP_OK;
    read_error = ESP_FAIL;
    assert(solar_os_tca8418_load_keymap("keyboard0", "/map.json") == ESP_FAIL);
    read_error = ESP_OK;
    file("{}");
    assert(solar_os_tca8418_load_keymap("keyboard0", "/map.json") == ESP_ERR_INVALID_ARG);
    static const char embedded_null[] = "{\"schema\":1,\"keys\":[]}\0trailing";
    contents = embedded_null; content_length = sizeof(embedded_null) - 1U;
    assert(solar_os_tca8418_load_keymap("keyboard0", "/map.json") == ESP_ERR_INVALID_ARG);
    char large[16385];
    memset(large, ' ', sizeof(large));
    contents = large; content_length = sizeof(large);
    assert(solar_os_tca8418_load_keymap("keyboard0", "/map.json") == ESP_ERR_INVALID_SIZE);
    assert(applies == 2);
    puts("TCA8418 keymap file tests: ok");
    return 0;
}
