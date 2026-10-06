#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Exercise the actual worker, polling, attach and detach implementation. */
#include "../../src/services/solar_os_inputronic_keyboard.c"
#include "../../src/services/solar_os_inputronic_keyboard_driver.c"
#include "solar_os_keys.h"

static uint8_t registers[0x30], fifo[32];
static size_t fifo_count, event_count, released, opened, closed, ready_changes;
static solar_os_input_key_event_t events[64];
static esp_err_t bus_error, input_error;
static bool task_failure, wait_timeout, overflow_during_read, refill_fifo;
static unsigned worker_ticks, worker_limit;
static uint8_t composition_modifiers;
static unsigned matrix_configurations;
static uint8_t fail_write_reg;
static bool fail_source_open;
static bool check_reset_before_probe;
static esp_err_t gpio_error;
static unsigned reset_delays, reset_cleanup, irq_cleanup;
static uint32_t reset_level;

esp_err_t gpio_config(const gpio_config_t *config)
{
    assert(config->intr_type == GPIO_INTR_DISABLE);
    if (config->mode == GPIO_MODE_INPUT) {
        assert(config->pin_bit_mask == (1ULL << 40));
        assert(config->pull_up_en == GPIO_PULLUP_ENABLE);
    } else {
        assert(config->mode == GPIO_MODE_OUTPUT);
        assert(config->pin_bit_mask == (1ULL << 41) && reset_level == 0);
    }
    return gpio_error;
}

esp_err_t gpio_set_level(gpio_num_t pin, uint32_t level)
{
    assert(pin == 41);
    reset_level = level;
    return gpio_error;
}

esp_err_t gpio_reset_pin(gpio_num_t pin)
{
    assert(pin == 40 || pin == 41);
    if (pin == 41) reset_cleanup++;
    else irq_cleanup++;
    return ESP_OK;
}

void esp_rom_delay_us(uint32_t us)
{
    assert(us >= 120U);
    assert(reset_level == (reset_delays % 2U));
    reset_delays++;
}

size_t strlcpy(char *dst, const char *src, size_t size)
{
    size_t length = strlen(src);
    if (size != 0U) {
        size_t copied = length < size - 1U ? length : size - 1U;
        memcpy(dst, src, copied);
        dst[copied] = '\0';
    }
    return length;
}

bool solar_os_expansion_find_i2c_bus(const char *name,
    solar_os_expansion_i2c_bus_t *bus, size_t *index)
{
    (void)bus; (void)index;
    return strcmp(name, "i2c0") == 0 || strcmp(name, "i2c1") == 0;
}

esp_err_t solar_os_bus_i2c_probe(const char *name, uint8_t address)
{
    assert(name != NULL && address == 0x34U);
    if (check_reset_before_probe) assert(reset_level == 1U && reset_delays >= 2U);
    return bus_error;
}

esp_err_t solar_os_bus_i2c_read_reg(const char *name, uint8_t address,
    uint8_t reg, uint8_t *data, size_t length)
{
    assert(name != NULL && address == 0x34U && length == 1U);
    if (bus_error != ESP_OK) return bus_error;
    if (reg == REG_KEY_COUNT) *data = (uint8_t)fifo_count;
    else if (reg == REG_KEY_EVENT) {
        assert(fifo_count != 0U);
        *data = fifo[0];
        memmove(fifo, fifo + 1, --fifo_count);
        if (refill_fifo) fifo[fifo_count++] = 0x80 | 52;
        if (overflow_during_read) {
            registers[REG_INT_STAT] |= INT_OVERFLOW;
            overflow_during_read = false;
        }
    } else *data = registers[reg];
    return ESP_OK;
}

esp_err_t solar_os_bus_i2c_write_reg(const char *name, uint8_t address,
    uint8_t reg, const uint8_t *data, size_t length)
{
    assert(name != NULL && address == 0x34U && length == 1U);
    if (bus_error != ESP_OK) return bus_error;
    if (reg == fail_write_reg) return ESP_FAIL;
    if (reg == REG_INT_STAT) registers[reg] &= (uint8_t)~*data;
    else registers[reg] = *data;
    if (reg == REG_CFG && *data == 0x29U) matrix_configurations++;
    return ESP_OK;
}

esp_err_t solar_os_input_keyboard_source_open(const char *name, bool ready,
    solar_os_input_source_t *source)
{
    assert(name != NULL && ready);
    if (fail_source_open) return ESP_ERR_NO_MEM;
    *source = (solar_os_input_source_t)++opened;
    return ESP_OK;
}

void solar_os_input_source_close(solar_os_input_source_t source)
{
    assert(source != 0U);
    closed++;
}

void solar_os_input_source_release_all(solar_os_input_source_t source)
{
    assert(source != 0U);
    released++;
}

esp_err_t solar_os_input_keyboard_source_set_ready(solar_os_input_source_t source, bool ready)
{
    assert(source != 0U);
    assert(ready == (ready_changes != 0U));
    ready_changes++;
    return ESP_OK;
}

void solar_os_input_composition_set_modifiers(solar_os_input_source_t source, uint8_t modifiers)
{
    assert(source != 0U);
    composition_modifiers = modifiers;
}

uint8_t solar_os_input_composition_apply(uint8_t key) { return key; }

/* The common translator itself is covered by input_test. Verify what this
 * driver passes to it; exercise a letter and modifier navigation here. */
uint8_t solar_os_input_translate_hid_usage(uint16_t usage, uint8_t modifiers, bool caps)
{
    if (usage >= 0x04U && usage <= 0x1dU) {
        uint8_t key = (uint8_t)('a' + usage - 0x04U);
        if ((modifiers & SOLAR_OS_INPUT_MOD_CTRL) != 0U) return (uint8_t)(key - 'a' + 1U);
        if (((modifiers & SOLAR_OS_INPUT_MOD_SHIFT) != 0U) != caps) key -= 'a' - 'A';
        return key;
    }
    if (usage == 0x52U) return (modifiers & SOLAR_OS_INPUT_MOD_SHIFT) != 0U ?
        SOLAR_OS_KEY_SHIFT_UP : SOLAR_OS_KEY_UP;
    return 0U;
}

esp_err_t solar_os_input_write_key(solar_os_input_source_t source, uint16_t physical,
    uint16_t usage, uint8_t key, uint8_t modifiers, solar_os_input_key_action_t action)
{
    assert(source != 0U);
    if (input_error != ESP_OK) return input_error;
    assert(event_count < sizeof(events) / sizeof(events[0]));
    events[event_count++] = (solar_os_input_key_event_t) {
        .source = source, .physical_key = physical, .usage = usage, .key = key,
        .modifiers = modifiers, .action = action,
    };
    return ESP_OK;
}

BaseType_t solar_os_task_create_pinned_internal(TaskFunction_t function, const char *name,
    uint32_t stack, void *arg, UBaseType_t priority, TaskHandle_t *task,
    BaseType_t core, solar_os_task_role_t role)
{
    assert(function == worker && name != NULL && stack == INPUTRONIC_TASK_STACK);
    (void)priority; (void)core; (void)role;
    *task = arg;
    return task_failure ? pdFALSE : pdPASS;
}

void solar_os_task_delete_internal(TaskHandle_t task) { assert(task == NULL); }
BaseType_t xTaskNotifyGive(TaskHandle_t task) { assert(task != NULL); return pdPASS; }

bool solar_os_task_wait_done(TaskHandle_t task, volatile bool *done, uint32_t timeout)
{
    assert(timeout == SOLAR_OS_TASK_STOP_WAIT_MS);
    if (wait_timeout) return false;
    worker(task);
    return *done;
}

uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t ticks)
{
    assert(clear == pdTRUE);
    assert(ticks == (worker_ticks == 0U ? INPUTRONIC_RETRY_MS : INPUTRONIC_POLL_MS));
    worker_ticks++;
    /* A failed first poll recovers on the next iteration, then accepts a
     * fresh key. No held Shift survives the connection interruption. */
    if (worker_ticks == 1U) bus_error = ESP_OK;
    if (worker_ticks == 2U) { fifo[0] = 0x80U | 52U; fifo_count = 1U; }
    if (worker_ticks >= worker_limit) devices[0].stop_requested = true;
    return 0U;
}

static void queue(uint8_t raw)
{
    assert(fifo_count < sizeof(fifo));
    fifo[fifo_count++] = raw;
    registers[REG_INT_STAT] |= INT_KEY;
}

static void test_codec(void)
{
    solar_os_inputronic_keyboard_state_t state = {0};
    solar_os_inputronic_key_transition_t transition;
    assert(!solar_os_inputronic_keyboard_decode(NULL, 0x80, &transition));
    assert(!solar_os_inputronic_keyboard_decode(&state, 0, &transition));
    assert(!solar_os_inputronic_keyboard_decode(&state, 0xd1, &transition));
    assert(!solar_os_inputronic_keyboard_decode(&state, 0x81, &transition));
    assert(!solar_os_inputronic_keyboard_decode(&state, 52, &transition));
    assert(solar_os_inputronic_keyboard_decode(&state, 0x80 | 74, &transition));
    assert(state.caps_lock && transition.usage == 0x39);
    assert(!solar_os_inputronic_keyboard_decode(&state, 0x80 | 74, &transition));
    assert(state.caps_lock);
    assert(solar_os_inputronic_keyboard_decode(&state, 74, &transition));
    assert(solar_os_inputronic_keyboard_decode(&state, 0x80 | 75, &transition));
    assert(transition.modifiers == SOLAR_OS_INPUT_MOD_LEFT_SHIFT);
    for (uint8_t id = 31; id <= 40; id++) {
        static const char expected[] = "=!\"#$%&/()";
        assert(solar_os_inputronic_keyboard_decode(&state, 0x80 | id, &transition));
        assert(transition.key == expected[id - 31]);
    }
    assert(solar_os_inputronic_keyboard_decode(&state, 0x80 | 51, &transition));
    assert(transition.key == ':');
    assert(solar_os_inputronic_keyboard_decode(&state, 0x80 | 69, &transition));
    assert(transition.key == ';');
    assert(solar_os_inputronic_keyboard_decode(&state, 0x80 | 70, &transition));
    assert(transition.key == ':');
    assert(solar_os_inputronic_keyboard_decode(&state, 0x80 | 76, &transition));
    assert(transition.modifiers == (SOLAR_OS_INPUT_MOD_LEFT_SHIFT | SOLAR_OS_INPUT_MOD_LEFT_CTRL));
    assert(solar_os_inputronic_keyboard_decode(&state, 0x80 | 77, &transition));
    assert(transition.modifiers == 7U);
    solar_os_inputronic_keyboard_release(&state);
    assert(state.caps_lock && state.modifiers == 0U && !state.held[75]);
    for (uint8_t id = 17; id <= 20; id++) {
        assert(solar_os_inputronic_keyboard_decode(&state, 0x80 | id, &transition));
        assert(transition.physical_key == id && transition.key == 0 && transition.usage == 0);
    }
    assert(solar_os_inputronic_keyboard_decode(&state, 0x80 | 78, &transition));
    assert(solar_os_inputronic_keyboard_decode(&state, 0x80 | 79, &transition));
    for (uint8_t id = 21; id <= 30; id++) {
        assert(solar_os_inputronic_keyboard_decode(&state, 0x80 | id, &transition));
        assert(transition.usage == 0x3a + id - 21);
    }
    /* Every printable letter's physical ID is independent of its HID usage. */
    static const uint8_t ids[] = {52,66,64,54,44,55,56,57,49,58,59,60,68,67,50,41,42,45,53,46,48,65,43,63,47,62};
    for (size_t i = 0; i < sizeof(ids); i++) {
        assert(solar_os_inputronic_keyboard_decode(&state, 0x80 | ids[i], &transition));
        assert(transition.usage == 4U + i);
    }
}

int main(void)
{
    test_codec();
    solar_os_expansion_binding_t bindings[] = {
        {.kind = SOLAR_OS_EXPANSION_BINDING_I2C_BUS, .target = "i2c0"},
        {.kind = SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS, .value = 0x34},
    };
    assert(solar_os_inputronic_keyboard_attach(NULL, bindings, 2) == ESP_ERR_INVALID_ARG);
    assert(solar_os_inputronic_keyboard_attach("keyboard0", bindings, 1) == ESP_ERR_INVALID_ARG);
    bindings[1].value = 0x35;
    assert(solar_os_inputronic_keyboard_attach("keyboard0", bindings, 2) == ESP_ERR_INVALID_ARG);
    bindings[1].value = 0x34;
    bus_error = ESP_FAIL;
    assert(solar_os_inputronic_keyboard_attach("keyboard0", bindings, 2) == ESP_FAIL);
    assert(opened == 0U);
    bus_error = ESP_OK;
    fail_write_reg = 0x1f;
    assert(solar_os_inputronic_keyboard_attach("keyboard0", bindings, 2) == ESP_FAIL);
    assert(opened == 0U && !devices[0].active);
    fail_write_reg = 0U;
    fail_source_open = true;
    assert(solar_os_inputronic_keyboard_attach("keyboard0", bindings, 2) == ESP_ERR_NO_MEM);
    assert(opened == 0U && !devices[0].active);
    fail_source_open = false;
    queue(0x80 | 52); /* Attach discards stale events. */
    task_failure = true;
    assert(solar_os_inputronic_keyboard_attach("keyboard0", bindings, 2) == ESP_ERR_NO_MEM);
    assert(opened == closed && !devices[0].active);
    task_failure = false;
    assert(solar_os_inputronic_keyboard_expansion_driver.attach("keyboard0", bindings, 2) == ESP_OK);
    assert(registers[0x1d] == 0xff && registers[0x1e] == 0xff && registers[0x1f] == 3);
    assert(registers[REG_CFG] == 0x29 && registers[0x29] == 0 && fifo_count == 0);
    assert(solar_os_inputronic_keyboard_attach("keyboard0", bindings, 2) == ESP_ERR_INVALID_STATE);
    inputronic_device_t *device = &devices[0];
    queue(0x80 | 75); queue(0x80 | 52); queue(52); queue(75);
    assert(poll_once(device) == ESP_OK);
    assert(event_count == 4 && events[1].key == 'A' && events[1].usage == 4);
    assert(events[2].action == SOLAR_OS_INPUT_KEY_RELEASE && composition_modifiers == 0);
    queue(0x80 | 76); queue(0x80 | 52); queue(52); queue(76);
    assert(poll_once(device) == ESP_OK && events[5].key == 1);
    queue(0x80 | 74); queue(74); queue(0x80 | 52); queue(52);
    assert(poll_once(device) == ESP_OK && events[10].key == 'A');
    queue(0x80 | 75); queue(0x80 | 7); queue(7); queue(75);
    assert(poll_once(device) == ESP_OK && events[13].key == SOLAR_OS_KEY_SHIFT_UP);
    queue(0x80 | 75); queue(0x80 | 52); queue(52); queue(75);
    assert(poll_once(device) == ESP_OK && events[17].key == 'a'); /* Caps XOR Shift. */
    queue(0x80 | 75);
    assert(poll_once(device) == ESP_OK && composition_modifiers != 0);
    size_t prior_events = event_count;
    queue(75); registers[REG_INT_STAT] |= INT_OVERFLOW;
    assert(poll_once(device) == ESP_OK && event_count == prior_events);
    assert(device->overflows == 1 && released == 1 && composition_modifiers == 0);
    assert(device->keyboard.caps_lock && fifo_count == 0);
    queue(0x80 | 52); overflow_during_read = true;
    assert(poll_once(device) == ESP_OK && event_count == prior_events);
    assert(device->overflows == 2 && released == 2);
    queue(0x80 | 52); refill_fifo = true;
    assert(discard_fifo(device) == ESP_ERR_TIMEOUT);
    refill_fifo = false;
    assert(discard_fifo(device) == ESP_OK && fifo_count == 0);
    registers[REG_INT_STAT] = 0;
    /* Count is polled even when K_INT was acknowledged concurrently. */
    queue(0x80 | 52); registers[REG_INT_STAT] = 0;
    assert(poll_once(device) == ESP_OK && events[event_count - 1].key == 'A');
    queue(52);
    assert(poll_once(device) == ESP_OK);
    input_error = ESP_ERR_NO_MEM; queue(0x80 | 75);
    assert(poll_once(device) == ESP_ERR_NO_MEM);
    input_error = ESP_OK;
    unsigned prior_configurations = matrix_configurations;
    bus_error = ESP_FAIL; worker_limit = 3;
    worker(device);
    assert(ready_changes == 2 && matrix_configurations == prior_configurations + 1);
    assert(device->keyboard.modifiers == 0 && composition_modifiers == 0);
    assert(events[event_count - 1].key == 'A');
    /* Multiple independent instances on different named buses. */
    strlcpy(bindings[0].target, "i2c1", sizeof(bindings[0].target));
    assert(solar_os_inputronic_keyboard_attach("keyboard1", bindings, 2) == ESP_OK);
    assert(!devices[1].keyboard.caps_lock && devices[1].source != device->source);
    wait_timeout = true;
    assert(solar_os_inputronic_keyboard_detach("keyboard1") == ESP_ERR_TIMEOUT);
    assert(devices[1].active); /* Registry must retain the bus/address lease. */
    wait_timeout = false;
    assert(solar_os_inputronic_keyboard_detach("keyboard1") == ESP_OK);
    assert(solar_os_inputronic_keyboard_detach("keyboard0") == ESP_OK);
    assert(opened == closed);
    assert(solar_os_inputronic_keyboard_detach("missing") == ESP_ERR_NOT_FOUND);

    /* A keyboard held in reset must be released BEFORE its first I2C probe.
     * Failed initialization and detach must relinquish both GPIOs. */
    solar_os_expansion_binding_t wired[] = {
        {.kind = SOLAR_OS_EXPANSION_BINDING_I2C_BUS, .target = "i2c0"},
        {.kind = SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS, .value = 0x34},
        {.kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "reset", .value = 41},
        {.kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "irq", .value = 40},
    };
    check_reset_before_probe = true;
    bus_error = ESP_ERR_NOT_FOUND;
    assert(solar_os_inputronic_keyboard_attach("wired", wired, 4) == ESP_ERR_NOT_FOUND);
    assert(reset_delays == 2U && reset_cleanup == 1U && irq_cleanup == 1U);
    gpio_error = ESP_FAIL;
    assert(solar_os_inputronic_keyboard_attach("wired", wired, 4) == ESP_FAIL);
    assert(reset_delays == 2U && reset_cleanup == 2U && irq_cleanup == 2U);
    gpio_error = ESP_OK;
    bus_error = ESP_OK;
    assert(solar_os_inputronic_keyboard_attach("wired", wired, 4) == ESP_OK);
    assert(reset_delays == 4U && reset_level == 1U);
    assert(solar_os_inputronic_keyboard_detach("wired") == ESP_OK);
    assert(reset_cleanup == 3U && irq_cleanup == 3U && opened == closed);
    wired[2].value = -1;
    assert(solar_os_inputronic_keyboard_attach("wired", wired, 4) == ESP_ERR_INVALID_ARG);
    wired[2].value = 40;
    assert(solar_os_inputronic_keyboard_attach("wired", wired, 4) == ESP_ERR_INVALID_ARG);
    puts("inputronic keyboard tests: ok");
    return 0;
}
