#include "solar_os_drv8830.h"

#include <string.h>
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "solar_os_buses.h"
#include "solar_os_gpio_controller.h"
#include "solar_os_memory.h"

typedef struct device {
    struct device *next;
    solar_os_drv8830_info_t info;
    solar_os_drv8830_chip_t chip;
} device_t;

static device_t *devices;
static StaticSemaphore_t mutex_storage;
static SemaphoreHandle_t mutex;
static portMUX_TYPE init_lock = portMUX_INITIALIZER_UNLOCKED;

/* Serialize transfers with detach so no callback can use released state. */
static void lock(void)
{
    portENTER_CRITICAL(&init_lock);
    if (mutex == NULL) mutex = xSemaphoreCreateMutexStatic(&mutex_storage);
    portEXIT_CRITICAL(&init_lock);
    xSemaphoreTake(mutex, portMAX_DELAY);
}

static device_t *find(const char *name)
{
    for (device_t *d = devices; d != NULL; d = d->next) {
        if (name != NULL && strcmp(name, d->info.name) == 0) return d;
    }
    return NULL;
}

static bool speed_supported(const device_t *d)
{
    solar_os_bus_info_t info;
    return solar_os_bus_find(d->info.bus, SOLAR_OS_BUS_PROTOCOL_I2C, &info) &&
        info.config.i2c.speed_hz > 0U && info.config.i2c.speed_hz <= 400000U;
}

static esp_err_t read_reg(void *ctx, uint8_t reg, uint8_t *data, size_t len)
{
    const device_t *d = ctx;
    if (!speed_supported(d)) return ESP_ERR_INVALID_STATE;
    return solar_os_bus_i2c_read_reg(d->info.bus, d->info.address, reg, data, len);
}

static esp_err_t write_reg(void *ctx, uint8_t reg, const uint8_t *data, size_t len)
{
    const device_t *d = ctx;
    if (!speed_supported(d)) return ESP_ERR_INVALID_STATE;
    return solar_os_bus_i2c_write_reg(d->info.bus, d->info.address, reg, data, len);
}

static esp_err_t parse(device_t *d, const solar_os_expansion_binding_t *bindings, size_t count)
{
    bool bus = false, address = false;
    const char *address_bus = NULL;
    d->info.interrupt_gpio = -1;
    if (bindings == NULL) return ESP_ERR_INVALID_ARG;
    for (size_t i = 0; i < count; i++) {
        const solar_os_expansion_binding_t *b = &bindings[i];
        if (b->kind == SOLAR_OS_EXPANSION_BINDING_I2C_BUS && !bus && b->target[0] != '\0') {
            if (strlcpy(d->info.bus, b->target, sizeof(d->info.bus)) >= sizeof(d->info.bus)) return ESP_ERR_INVALID_ARG;
            bus = true;
        } else if (b->kind == SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS && !address && b->value >= 0x60 && b->value <= 0x68) {
            d->info.address = (uint8_t)b->value;
            address_bus = b->target;
            address = true;
        } else if (b->kind == SOLAR_OS_EXPANSION_BINDING_GPIO && strcmp(b->role, "int") == 0 &&
                   d->info.interrupt_gpio < 0 && GPIO_IS_VALID_GPIO(b->value)) {
            d->info.interrupt_gpio = b->value;
        } else return ESP_ERR_INVALID_ARG;
    }
    return bus && address && (address_bus[0] == '\0' || strcmp(address_bus, d->info.bus) == 0) &&
        speed_supported(d) ? ESP_OK : ESP_ERR_INVALID_ARG;
}

esp_err_t solar_os_drv8830_attach(const char *name, const solar_os_expansion_binding_t *bindings, size_t count)
{
    if (name == NULL || name[0] == '\0' || strnlen(name, SOLAR_OS_EXPANSION_DEVICE_NAME_MAX) >= SOLAR_OS_EXPANSION_DEVICE_NAME_MAX) return ESP_ERR_INVALID_ARG;
    lock();
    esp_err_t ret = find(name) == NULL ? ESP_OK : ESP_ERR_INVALID_STATE;
    device_t *d = NULL;
    if (ret == ESP_OK) {
        d = solar_os_memory_calloc(1U, sizeof(*d), SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "drv8830.device");
        if (d == NULL) ret = ESP_ERR_NO_MEM;
    }
    if (ret == ESP_OK) ret = parse(d, bindings, count);
    bool configured = false;
    if (ret == ESP_OK && d->info.interrupt_gpio >= 0) {
        const solar_os_gpio_line_ref_t line = {.line = (uint8_t)d->info.interrupt_gpio};
        configured = true;
        ret = solar_os_gpio_line_configure(&line, SOLAR_OS_GPIO_LINE_MODE_INPUT, SOLAR_OS_GPIO_LINE_PULL_UP);
    }
    if (ret == ESP_OK) ret = solar_os_bus_i2c_probe(d->info.bus, d->info.address);
    if (ret == ESP_OK) {
        const solar_os_drv8830_io_t io = {.read = read_reg, .write = write_reg, .ctx = d};
        ret = solar_os_drv8830_chip_init(&d->chip, &io);
    }
    if (ret == ESP_OK) {
        strlcpy(d->info.name, name, sizeof(d->info.name));
        d->next = devices;
        devices = d;
    } else if (d != NULL) {
        if (configured) (void)gpio_reset_pin((gpio_num_t)d->info.interrupt_gpio);
        solar_os_memory_free(d);
    }
    xSemaphoreGive(mutex);
    return ret;
}

esp_err_t solar_os_drv8830_detach(const char *name)
{
    if (name == NULL) return ESP_ERR_INVALID_ARG;
    lock();
    esp_err_t ret = ESP_ERR_NOT_FOUND;
    for (device_t **link = &devices; *link != NULL; link = &(*link)->next) {
        device_t *d = *link;
        if (strcmp(d->info.name, name) != 0) continue;
        ret = solar_os_drv8830_chip_stop(&d->chip, false);
        if (ret == ESP_OK && d->info.interrupt_gpio >= 0) ret = gpio_reset_pin((gpio_num_t)d->info.interrupt_gpio);
        if (ret == ESP_OK) { *link = d->next; solar_os_memory_free(d); }
        break;
    }
    xSemaphoreGive(mutex);
    return ret;
}

bool solar_os_drv8830_get(size_t index, solar_os_drv8830_info_t *info)
{
    if (info == NULL) return false;
    lock();
    device_t *d = devices;
    while (d != NULL && index > 0U) { d = d->next; index--; }
    if (d != NULL) *info = d->info;
    const bool found = d != NULL;
    xSemaphoreGive(mutex);
    return found;
}

static esp_err_t fault_line(device_t *d, bool *asserted)
{
    *asserted = false;
    if (d->info.interrupt_gpio < 0) return ESP_OK;
    bool level;
    const solar_os_gpio_line_ref_t line = {.line = (uint8_t)d->info.interrupt_gpio};
    const esp_err_t ret = solar_os_gpio_line_read(&line, &level);
    if (ret == ESP_OK) *asserted = !level;
    return ret;
}

esp_err_t solar_os_drv8830_status(const char *name, solar_os_drv8830_status_t *status)
{
    if (name == NULL || status == NULL) return ESP_ERR_INVALID_ARG;
    lock();
    device_t *d = find(name);
    solar_os_drv8830_status_t value = {0};
    esp_err_t ret = d == NULL ? ESP_ERR_NOT_FOUND : solar_os_drv8830_chip_status(&d->chip, &value.chip);
    if (ret == ESP_OK) {
        value.interrupt_present = d->info.interrupt_gpio >= 0;
        ret = fault_line(d, &value.fault_asserted);
    }
    if (ret == ESP_OK) *status = value;
    xSemaphoreGive(mutex);
    return ret;
}

esp_err_t solar_os_drv8830_drive(const char *name, int voltage_mv)
{
    if (name == NULL) return ESP_ERR_INVALID_ARG;
    lock();
    device_t *d = find(name);
    bool asserted = false;
    esp_err_t ret = d == NULL ? ESP_ERR_NOT_FOUND : ESP_OK;
    if (ret == ESP_OK && voltage_mv != 0) ret = fault_line(d, &asserted);
    if (ret == ESP_OK && asserted) ret = ESP_ERR_INVALID_STATE;
    if (ret == ESP_OK) ret = solar_os_drv8830_chip_drive(&d->chip, voltage_mv);
    xSemaphoreGive(mutex);
    return ret;
}

esp_err_t solar_os_drv8830_stop(const char *name, bool brake)
{
    if (name == NULL) return ESP_ERR_INVALID_ARG;
    lock();
    device_t *d = find(name);
    const esp_err_t ret = d == NULL ? ESP_ERR_NOT_FOUND : solar_os_drv8830_chip_stop(&d->chip, brake);
    xSemaphoreGive(mutex);
    return ret;
}

esp_err_t solar_os_drv8830_clear(const char *name)
{
    if (name == NULL) return ESP_ERR_INVALID_ARG;
    lock();
    device_t *d = find(name);
    const esp_err_t ret = d == NULL ? ESP_ERR_NOT_FOUND : solar_os_drv8830_chip_clear(&d->chip);
    xSemaphoreGive(mutex);
    return ret;
}
