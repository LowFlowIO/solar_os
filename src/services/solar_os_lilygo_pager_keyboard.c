#include "solar_os_lilygo_pager_keyboard.h"

#include <string.h>
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "pwm_port.h"
#include "solar_os_tca8418.h"
#include "solar_os_memory.h"
#include <stdlib.h>

/* Matrix positions and printed symbol legends from the existing Pager
 * profile. Controller transactions and input state live in the shared core. */
void solar_os_lilygo_pager_keyboard_map(solar_os_matrix_keyboard_map_t *map)
{
    static const uint8_t usages[4][10] = {
        {0x14,0x1a,0x08,0x15,0x17,0x1c,0x18,0x0c,0x12,0x13},
        {0x04,0x16,0x07,0x09,0x0a,0x0b,0x0d,0x0e,0x0f,0x28},
        {0xe2,0x1d,0x1b,0x06,0x19,0x05,0x11,0x10,0xe1,0x2a},
        {0x2c,0,0,0,0,0,0,0,0,0},
    };
    static const char symbols[4][10] = {
        {'1','2','3','4','5','6','7','8','9','0'},
        {'*','/','+','-','=',':','\'','"','@',0x1b},
        {0,'_','$',';','?','!',',','.',0,0},
        {0,0,0,0,0,0,0,0,0,0},
    };
    solar_os_matrix_keyboard_init_map(map, 4U, 10U);
    for (unsigned id = 1; id <= 40; id++) {
        const unsigned row = (id - 1U) / 10U, col = (id - 1U) % 10U;
        map->keys[0][id].usage = usages[row][col];
        /* Unlabelled cells are unused rather than phantom Space presses. */
        if (usages[row][col] == 0U) continue;
        map->keys[1][id] = map->keys[0][id];
        if (symbols[row][col] != 0) {
            map->keys[1][id].usage = 0;
            map->keys[1][id].key = (uint8_t)symbols[row][col];
        }
    }
    map->keys[0][31].flags = SOLAR_OS_MATRIX_KEY_LAYER_TAP;
    memset(&map->keys[1][31], 0, sizeof(map->keys[1][31]));
    /* Existing Alt+B reservation: no text and no new backlight action. */
    map->keys[0][26].flags = SOLAR_OS_MATRIX_KEY_ALT_BLOCK;
    map->keys[1][26].flags = SOLAR_OS_MATRIX_KEY_ALT_BLOCK;
}

#define PAGER_DEVICE_MAX 4U
typedef struct {
    bool active, controller_attached;
    char name[SOLAR_OS_EXPANSION_DEVICE_NAME_MAX];
    int backlight_pin;
} pager_device_t;
static EXT_RAM_BSS_ATTR pager_device_t pager_devices[PAGER_DEVICE_MAX];
static StaticSemaphore_t pager_mutex_buffer;
static SemaphoreHandle_t pager_mutex;
static portMUX_TYPE pager_init_lock = portMUX_INITIALIZER_UNLOCKED;

static bool take_pager_mutex(void)
{
    portENTER_CRITICAL(&pager_init_lock);
    if (pager_mutex == NULL) pager_mutex = xSemaphoreCreateMutexStatic(&pager_mutex_buffer);
    portEXIT_CRITICAL(&pager_init_lock);
    return pager_mutex != NULL && xSemaphoreTake(pager_mutex, portMAX_DELAY) == pdTRUE;
}

static esp_err_t attach_pager_locked(const char *name,
    const solar_os_expansion_binding_t *bindings, size_t count)
{
    if (name == NULL || name[0] == '\0' ||
        strlen(name) >= SOLAR_OS_EXPANSION_DEVICE_NAME_MAX || bindings == NULL ||
        count > SOLAR_OS_EXPANSION_DEVICE_BINDING_MAX) return ESP_ERR_INVALID_ARG;
    pager_device_t *device = NULL;
    for (unsigned i = 0; i < PAGER_DEVICE_MAX; i++) {
        if (pager_devices[i].active && strcmp(pager_devices[i].name, name) == 0)
            return ESP_ERR_INVALID_STATE;
        if (!pager_devices[i].active && device == NULL) device = &pager_devices[i];
    }
    if (device == NULL) return ESP_ERR_NO_MEM;
    int backlight_pin = -1;
    solar_os_expansion_binding_t controller_bindings[SOLAR_OS_EXPANSION_DEVICE_BINDING_MAX];
    size_t controller_count = 0;
    for (size_t i = 0; i < count; i++) {
        if (bindings[i].kind == SOLAR_OS_EXPANSION_BINDING_PWM) {
            if (strcmp(bindings[i].role, "backlight") != 0 || backlight_pin >= 0 ||
                bindings[i].value < 0) return ESP_ERR_INVALID_ARG;
            backlight_pin = bindings[i].value;
        } else {
            controller_bindings[controller_count++] = bindings[i];
        }
    }
    solar_os_matrix_keyboard_map_t *map = solar_os_memory_alloc(sizeof(*map),
        SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "keyboard-map");
    if (map == NULL) return ESP_ERR_NO_MEM;
    solar_os_lilygo_pager_keyboard_map(map);
    esp_err_t err = ESP_OK;
    if (backlight_pin >= 0) err = pwm_port_set(backlight_pin, 5000U, 50U);
    if (err == ESP_OK)
        err = solar_os_tca8418_attach_profile(name, controller_bindings, controller_count, map);
    free(map);
    if (err != ESP_OK) {
        if (backlight_pin >= 0) (void)pwm_port_stop(backlight_pin);
        return err;
    }
    strlcpy(device->name, name, sizeof(device->name));
    device->backlight_pin = backlight_pin;
    device->controller_attached = true;
    device->active = true;
    return ESP_OK;
}

static esp_err_t detach_pager_locked(const char *name)
{
    if (name == NULL) return ESP_ERR_INVALID_ARG;
    for (unsigned i = 0; i < PAGER_DEVICE_MAX; i++) {
        pager_device_t *device = &pager_devices[i];
        if (!device->active || strcmp(device->name, name) != 0) continue;
        if (device->controller_attached) {
            const esp_err_t err = solar_os_tca8418_detach(name);
            if (err != ESP_OK) return err;
            device->controller_attached = false;
        }
        if (device->backlight_pin >= 0) {
            const esp_err_t err = pwm_port_stop(device->backlight_pin);
            if (err != ESP_OK) return err;
        }
        memset(device, 0, sizeof(*device));
        return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t solar_os_lilygo_pager_keyboard_attach(const char *name,
    const solar_os_expansion_binding_t *bindings, size_t count)
{
    if (!take_pager_mutex()) return ESP_ERR_NO_MEM;
    const esp_err_t err = attach_pager_locked(name, bindings, count);
    xSemaphoreGive(pager_mutex);
    return err;
}

esp_err_t solar_os_lilygo_pager_keyboard_detach(const char *name)
{
    if (!take_pager_mutex()) return ESP_ERR_NO_MEM;
    const esp_err_t err = detach_pager_locked(name);
    xSemaphoreGive(pager_mutex);
    return err;
}
