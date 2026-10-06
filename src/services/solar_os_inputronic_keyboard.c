#include "solar_os_inputronic_keyboard.h"

#include <string.h>

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "solar_os_buses.h"
#include "solar_os_input.h"
#include "solar_os_input_composition.h"
#include "solar_os_inputronic_keyboard_codec.h"
#include "solar_os_task.h"

#define INPUTRONIC_DEVICE_MAX 4U
#define INPUTRONIC_POLL_MS 10U
#define INPUTRONIC_RETRY_MS 250U
#define INPUTRONIC_TASK_STACK 3072U
#define INPUTRONIC_FIFO_MAX 10U
#define INPUTRONIC_DRAIN_MAX 32U

#define REG_CFG 0x01U
#define REG_INT_STAT 0x02U
#define REG_KEY_COUNT 0x03U
#define REG_KEY_EVENT 0x04U
#define INT_KEY 0x01U
#define INT_OVERFLOW 0x08U

typedef struct {
    bool active;
    volatile bool stop_requested;
    volatile bool worker_done;
    char name[SOLAR_OS_EXPANSION_DEVICE_NAME_MAX];
    char bus[SOLAR_OS_EXPANSION_TARGET_MAX];
    solar_os_input_source_t source;
    TaskHandle_t task;
    solar_os_inputronic_keyboard_state_t keyboard;
    uint32_t transitions;
    uint32_t overflows;
    uint32_t errors;
    int reset_pin;
    int irq_pin;
} inputronic_device_t;

static const char *TAG = "inputronic_keyboard";
static EXT_RAM_BSS_ATTR inputronic_device_t devices[INPUTRONIC_DEVICE_MAX];

static void release_pins(inputronic_device_t *device)
{
    if (device->reset_pin >= 0) (void)gpio_reset_pin(device->reset_pin);
    if (device->irq_pin >= 0) (void)gpio_reset_pin(device->irq_pin);
}

static esp_err_t configure_pins(inputronic_device_t *device)
{
    if (device->irq_pin >= 0) {
        const gpio_config_t config = {
            .pin_bit_mask = 1ULL << (unsigned)device->irq_pin,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_RETURN_ON_ERROR(gpio_config(&config), TAG, "interrupt pin");
    }
    if (device->reset_pin >= 0) {
        const gpio_config_t config = {
            .pin_bit_mask = 1ULL << (unsigned)device->reset_pin,
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_RETURN_ON_ERROR(gpio_set_level(device->reset_pin, 0), TAG, "reset assert");
        ESP_RETURN_ON_ERROR(gpio_config(&config), TAG, "reset pin");
        /* SCPS215G specifies at least 120 us for reset and recovery. */
        esp_rom_delay_us(200U);
        ESP_RETURN_ON_ERROR(gpio_set_level(device->reset_pin, 1), TAG, "reset release");
        esp_rom_delay_us(200U);
    }
    return ESP_OK;
}

static esp_err_t read_reg(inputronic_device_t *device, uint8_t reg, uint8_t *value)
{
    return solar_os_bus_i2c_read_reg(device->bus,
        SOLAR_OS_INPUTRONIC_KEYBOARD_ADDRESS, reg, value, 1U);
}

static esp_err_t write_reg(inputronic_device_t *device, uint8_t reg, uint8_t value)
{
    return solar_os_bus_i2c_write_reg(device->bus,
        SOLAR_OS_INPUTRONIC_KEYBOARD_ADDRESS, reg, &value, 1U);
}

static void release_keys(inputronic_device_t *device)
{
    solar_os_input_source_release_all(device->source);
    solar_os_input_composition_set_modifiers(device->source, 0U);
    solar_os_inputronic_keyboard_release(&device->keyboard);
}

static esp_err_t discard_fifo(inputronic_device_t *device)
{
    for (size_t i = 0; i < INPUTRONIC_DRAIN_MAX; i++) {
        uint8_t count = 0, discard = 0;
        ESP_RETURN_ON_ERROR(read_reg(device, REG_KEY_COUNT, &count), TAG, "FIFO count");
        if ((count & 0x0fU) == 0U) return ESP_OK;
        ESP_RETURN_ON_ERROR(read_reg(device, REG_KEY_EVENT, &discard), TAG, "FIFO drain");
    }
    return ESP_ERR_TIMEOUT;
}

static esp_err_t configure_matrix(inputronic_device_t *device)
{
    /* TI TCA8418 SCPS215G: use all 8 rows and 10 columns, enable debounce,
     * disable GPIO events/interrupts and keypad lock. No host IRQ is needed.
     * Overflow erratum 8.6.4 requires BOTH OVR_FLOW_M and OVR_FLOW_IEN;
     * KE_IEN keeps key status available even though the INT pin is unused.
     * Auto-increment stays off: every transfer addresses a single register.
     */
    static const uint8_t setup[][2] = {
        {REG_CFG, 0x00},
        {0x1d, 0x00}, {0x1e, 0x00}, {0x1f, 0x00},
        {0x23, 0x00}, {0x24, 0x00}, {0x25, 0x00},
        {0x1a, 0x00}, {0x1b, 0x00}, {0x1c, 0x00},
        {0x20, 0x00}, {0x21, 0x00}, {0x22, 0x00},
        {0x29, 0x00}, {0x2a, 0x00}, {0x2b, 0x00},
        {0x2c, 0x00}, {0x2d, 0x00}, {0x2e, 0x00},
        {REG_KEY_COUNT, 0x00},
        {0x1d, 0xff}, {0x1e, 0xff}, {0x1f, 0x03},
        {REG_CFG, 0x29},
    };
    for (size_t i = 0; i < sizeof(setup) / sizeof(setup[0]); i++) {
        ESP_RETURN_ON_ERROR(write_reg(device, setup[i][0], setup[i][1]), TAG, "matrix setup");
    }
    ESP_RETURN_ON_ERROR(discard_fifo(device), TAG, "stale FIFO");
    return write_reg(device, REG_INT_STAT, 0x1fU);
}

static esp_err_t recover_overflow(inputronic_device_t *device)
{
    device->overflows++;
    release_keys(device);
    ESP_RETURN_ON_ERROR(discard_fifo(device), TAG, "overflow FIFO");
    return write_reg(device, REG_INT_STAT, INT_OVERFLOW | INT_KEY);
}

static esp_err_t poll_once(inputronic_device_t *device)
{
    uint8_t status = 0;
    ESP_RETURN_ON_ERROR(read_reg(device, REG_INT_STAT, &status), TAG, "key status");
    if ((status & INT_OVERFLOW) != 0U) return recover_overflow(device);

    /* Stage a bounded batch before publishing. Check overflow again so an
     * overflow during the read cannot leave a partially trusted held state.
     * Poll the count regardless of K_INT to avoid an interrupt-ack race.
     */
    uint8_t events[INPUTRONIC_FIFO_MAX];
    size_t event_count = 0;
    while (event_count < INPUTRONIC_FIFO_MAX) {
        uint8_t count = 0;
        ESP_RETURN_ON_ERROR(read_reg(device, REG_KEY_COUNT, &count), TAG, "key count");
        count &= 0x0fU;
        if (count > INPUTRONIC_FIFO_MAX) return ESP_ERR_INVALID_RESPONSE;
        if (count == 0U) break;
        ESP_RETURN_ON_ERROR(read_reg(device, REG_KEY_EVENT, &events[event_count]), TAG, "key event");
        event_count++;
    }
    ESP_RETURN_ON_ERROR(read_reg(device, REG_INT_STAT, &status), TAG, "overflow status");
    if ((status & INT_OVERFLOW) != 0U) return recover_overflow(device);

    for (size_t i = 0; i < event_count; i++) {
        solar_os_inputronic_key_transition_t transition;
        if (!solar_os_inputronic_keyboard_decode(&device->keyboard, events[i], &transition)) continue;
        solar_os_input_composition_set_modifiers(device->source, transition.modifiers);
        uint8_t key = transition.key;
        if (key == 0U && transition.usage != 0U) {
            key = solar_os_input_translate_hid_usage(transition.usage,
                transition.modifiers, device->keyboard.caps_lock);
        }
        key = solar_os_input_composition_apply(key);
        ESP_RETURN_ON_ERROR(solar_os_input_write_key(device->source,
            transition.physical_key, transition.usage, key, transition.modifiers,
            transition.pressed ? SOLAR_OS_INPUT_KEY_PRESS : SOLAR_OS_INPUT_KEY_RELEASE),
            TAG, "input queue");
        device->transitions++;
    }
    return write_reg(device, REG_INT_STAT, INT_KEY);
}

static void worker(void *arg)
{
    inputronic_device_t *device = arg;
    bool recovering = false;
    while (!device->stop_requested) {
        esp_err_t err = recovering ? configure_matrix(device) : poll_once(device);
        if (err != ESP_OK) {
            device->errors++;
            if (!recovering) {
                release_keys(device);
                (void)solar_os_input_keyboard_source_set_ready(device->source, false);
                ESP_LOGW(TAG, "%s input interrupted: %s", device->name, esp_err_to_name(err));
            }
            recovering = true;
        } else if (recovering) {
            (void)solar_os_input_keyboard_source_set_ready(device->source, true);
            recovering = false;
        }
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(
            recovering ? INPUTRONIC_RETRY_MS : INPUTRONIC_POLL_MS));
    }
    release_keys(device);
    device->worker_done = true;
    solar_os_task_delete_internal(NULL);
}

esp_err_t solar_os_inputronic_keyboard_attach(
    const char *name, const solar_os_expansion_binding_t *bindings, size_t binding_count)
{
    if (name == NULL || name[0] == '\0' || bindings == NULL ||
        strlen(name) >= SOLAR_OS_EXPANSION_DEVICE_NAME_MAX) return ESP_ERR_INVALID_ARG;
    inputronic_device_t *device = NULL;
    for (size_t i = 0; i < INPUTRONIC_DEVICE_MAX; i++) {
        if (devices[i].active && strcmp(devices[i].name, name) == 0) return ESP_ERR_INVALID_STATE;
        if (!devices[i].active && device == NULL) device = &devices[i];
    }
    if (device == NULL) return ESP_ERR_NO_MEM;

    const char *bus = NULL;
    bool have_address = false;
    int reset_pin = -1, irq_pin = -1;
    for (size_t i = 0; i < binding_count; i++) {
        if (bindings[i].kind == SOLAR_OS_EXPANSION_BINDING_I2C_BUS && bus == NULL) {
            bus = bindings[i].target;
        } else if (bindings[i].kind == SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS &&
                   !have_address && bindings[i].value == SOLAR_OS_INPUTRONIC_KEYBOARD_ADDRESS) {
            have_address = true;
        } else if (bindings[i].kind == SOLAR_OS_EXPANSION_BINDING_GPIO &&
                   strcmp(bindings[i].role, "reset") == 0 && reset_pin < 0 &&
                   GPIO_IS_VALID_OUTPUT_GPIO(bindings[i].value)) {
            reset_pin = bindings[i].value;
        } else if (bindings[i].kind == SOLAR_OS_EXPANSION_BINDING_GPIO &&
                   strcmp(bindings[i].role, "irq") == 0 && irq_pin < 0 &&
                   GPIO_IS_VALID_GPIO(bindings[i].value)) {
            irq_pin = bindings[i].value;
        } else return ESP_ERR_INVALID_ARG;
    }
    if (bus == NULL || !have_address ||
        (reset_pin >= 0 && reset_pin == irq_pin) ||
        !solar_os_expansion_find_i2c_bus(bus, NULL, NULL))
        return ESP_ERR_INVALID_ARG;
    memset(device, 0, sizeof(*device));
    device->reset_pin = reset_pin;
    device->irq_pin = irq_pin;
    strlcpy(device->name, name, sizeof(device->name));
    strlcpy(device->bus, bus, sizeof(device->bus));
    esp_err_t err = configure_pins(device);
    if (err == ESP_OK) err = solar_os_bus_i2c_probe(bus, SOLAR_OS_INPUTRONIC_KEYBOARD_ADDRESS);
    if (err == ESP_OK) err = configure_matrix(device);
    if (err != ESP_OK) {
        release_pins(device);
        ESP_RETURN_ON_ERROR(err, TAG, "keyboard initialization");
    }
    err = solar_os_input_keyboard_source_open(name, true, &device->source);
    if (err != ESP_OK) {
        release_pins(device);
        return err;
    }
    device->active = true;
    if (solar_os_task_create_pinned_internal(worker, name, INPUTRONIC_TASK_STACK,
            device, tskIDLE_PRIORITY + 1, &device->task, tskNO_AFFINITY,
            SOLAR_OS_TASK_ROLE_BACKGROUND) != pdPASS) {
        solar_os_input_source_close(device->source);
        release_pins(device);
        memset(device, 0, sizeof(*device));
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "%s attached on %s address 0x34", name, bus);
    return ESP_OK;
}

esp_err_t solar_os_inputronic_keyboard_detach(const char *name)
{
    if (name == NULL) return ESP_ERR_INVALID_ARG;
    for (size_t i = 0; i < INPUTRONIC_DEVICE_MAX; i++) {
        inputronic_device_t *device = &devices[i];
        if (!device->active || strcmp(device->name, name) != 0) continue;
        device->stop_requested = true;
        (void)xTaskNotifyGive(device->task);
        if (!solar_os_task_wait_done(device->task, &device->worker_done,
                                     SOLAR_OS_TASK_STOP_WAIT_MS)) return ESP_ERR_TIMEOUT;
        ESP_LOGI(TAG, "%s detached: %lu transitions, %lu overflows, %lu errors",
            name, (unsigned long)device->transitions, (unsigned long)device->overflows,
            (unsigned long)device->errors);
        solar_os_input_source_close(device->source);
        release_pins(device);
        memset(device, 0, sizeof(*device));
        return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}
