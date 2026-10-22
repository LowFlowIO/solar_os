#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "solar_os_drv8830.h"
#include "solar_os_buses.h"
#include "solar_os_gpio_controller.h"
#include "solar_os_memory.h"
#include "driver/gpio.h"

extern const solar_os_expansion_driver_t solar_os_drv8830_expansion_driver;
static uint8_t regs[2][9][2];
static unsigned allocations, probes, writes, resets;
static uint32_t speed = 100000U;
static int fail_read = -1, fail_write = -1;
static unsigned write_failures;
static bool fail_alloc, fail_probe, fail_configure, fail_reset, fail_line;
static bool line_level = true;
static uint8_t history[64];
static unsigned history_len;

size_t strlcpy(char *dst, const char *src, size_t size)
{
    const size_t len = strlen(src);
    if (size > 0U) { const size_t n = len < size - 1U ? len : size - 1U; memcpy(dst, src, n); dst[n] = '\0'; }
    return len;
}
void *solar_os_memory_calloc(size_t count, size_t size, solar_os_memory_class_t cls, const char *tag)
{
    assert(cls == SOLAR_OS_MEMORY_EXTERNAL_PREFERRED && strcmp(tag, "drv8830.device") == 0);
    if (fail_alloc) return NULL;
    void *ptr = calloc(count, size);
    if (ptr != NULL) allocations++;
    return ptr;
}
void solar_os_memory_free(void *ptr) { assert(ptr && allocations); allocations--; free(ptr); }
bool solar_os_bus_find(const char *name, solar_os_bus_protocol_t protocol, solar_os_bus_info_t *info)
{
    assert(protocol == SOLAR_OS_BUS_PROTOCOL_I2C);
    if (strcmp(name, "i2c0") != 0 && strcmp(name, "aux") != 0) return false;
    if (info) *info = (solar_os_bus_info_t){.config.i2c.speed_hz = speed};
    return true;
}
esp_err_t solar_os_bus_i2c_probe(const char *name, uint8_t address)
{
    assert(solar_os_bus_find(name, SOLAR_OS_BUS_PROTOCOL_I2C, NULL));
    assert(address >= 0x60 && address <= 0x68);
    probes++;
    return fail_probe ? ESP_FAIL : ESP_OK;
}
static uint8_t *registers(const char *name, uint8_t address)
{
    assert(address >= 0x60 && address <= 0x68);
    return regs[strcmp(name, "aux") == 0][address - 0x60];
}
esp_err_t solar_os_bus_i2c_read_reg(const char *name, uint8_t address, uint8_t reg, uint8_t *data, size_t len)
{
    assert(reg <= 1U && len == 1U && speed <= 400000U);
    if (reg == fail_read) return ESP_FAIL;
    *data = registers(name, address)[reg];
    return ESP_OK;
}
esp_err_t solar_os_bus_i2c_write_reg(const char *name, uint8_t address, uint8_t reg, const uint8_t *data, size_t len)
{
    assert(reg <= 1U && len == 1U && speed <= 400000U);
    writes++;
    if (reg == fail_write && write_failures > 0U) { write_failures--; return ESP_FAIL; }
    if (reg == 0 && history_len < sizeof(history)) history[history_len++] = *data;
    if (reg == 1) { assert(*data == 0x80); registers(name, address)[1] = 0; }
    else registers(name, address)[0] = *data;
    return ESP_OK;
}
esp_err_t solar_os_gpio_line_configure(const solar_os_gpio_line_ref_t *line,
    solar_os_gpio_line_mode_t mode, solar_os_gpio_line_pull_t pull)
{
    assert(line->controller[0] == '\0' && line->line == 1);
    assert(mode == SOLAR_OS_GPIO_LINE_MODE_INPUT && pull == SOLAR_OS_GPIO_LINE_PULL_UP);
    return fail_configure ? ESP_FAIL : ESP_OK;
}
esp_err_t solar_os_gpio_line_read(const solar_os_gpio_line_ref_t *line, bool *level)
{
    assert(line->line == 1);
    if (fail_line) return ESP_FAIL;
    *level = line_level;
    return ESP_OK;
}
esp_err_t gpio_reset_pin(gpio_num_t pin) { assert(pin == 1); resets++; return fail_reset ? ESP_FAIL : ESP_OK; }

int main(void)
{
    const solar_os_expansion_driver_t *driver = &solar_os_drv8830_expansion_driver;
    assert(strcmp(driver->name, "drv8830") == 0 && driver->probe_supported);
    assert(driver->required_capabilities == SOLAR_OS_BOARD_CAP_EXPANSION_I2C);
    assert(driver->binding_spec_count == 3U && !driver->binding_specs[2].required);
    solar_os_expansion_binding_t bindings[] = {
        {.kind = SOLAR_OS_EXPANSION_BINDING_I2C_BUS, .target = "i2c0"},
        {.kind = SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS, .value = 0x60},
        {.kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "int", .value = 1},
    };
    assert(driver->attach(NULL, bindings, 2) == ESP_ERR_INVALID_ARG);
    assert(driver->attach("", bindings, 2) == ESP_ERR_INVALID_ARG);
    assert(driver->attach("too_long_for_device_name", bindings, 2) == ESP_ERR_INVALID_ARG);
    fail_alloc = true;
    assert(driver->attach("motor0", bindings, 2) == ESP_ERR_NO_MEM && allocations == 0);
    fail_alloc = false;
    bindings[1].value = 0xc0;
    assert(driver->attach("motor0", bindings, 2) == ESP_ERR_INVALID_ARG && probes == 0);
    bindings[1].value = 0x60;
    strcpy(bindings[1].target, "aux");
    assert(driver->attach("motor0", bindings, 2) == ESP_ERR_INVALID_ARG && probes == 0);
    bindings[1].target[0] = '\0';
    assert(driver->attach("motor0", bindings, 1) == ESP_ERR_INVALID_ARG);
    speed = 1000000U;
    assert(driver->attach("motor0", bindings, 2) == ESP_ERR_INVALID_ARG);
    speed = 400000U;
    fail_probe = true;
    assert(driver->attach("motor0", bindings, 3) == ESP_FAIL && allocations == 0 && resets == 1);
    fail_probe = false;
    fail_configure = true;
    assert(driver->attach("motor0", bindings, 3) == ESP_FAIL && allocations == 0 && resets == 2);
    fail_configure = false;
    fail_write = 0; write_failures = 2;
    assert(driver->attach("motor0", bindings, 3) == ESP_FAIL && allocations == 0);
    fail_write = -1;

    /* Attaching an already running bridge must command coast. */
    regs[0][0][0] = 0xfd;
    assert(driver->attach("motor0", bindings, 3) == ESP_OK && regs[0][0][0] == 0);
    assert(driver->attach("motor0", bindings, 3) == ESP_ERR_INVALID_STATE && allocations == 1);
    strcpy(bindings[0].target, "aux");
    bindings[1].value = 0x68;
    assert(driver->attach("motor1", bindings, 2) == ESP_OK && allocations == 2);
    solar_os_drv8830_info_t info;
    assert(solar_os_drv8830_get(0, &info) && strcmp(info.bus, "aux") == 0 && info.interrupt_gpio == -1);
    assert(!solar_os_drv8830_get(2, &info));
    solar_os_drv8830_status_t status;
    assert(solar_os_drv8830_status("motor0", &status) == ESP_OK && status.interrupt_present && !status.fault_asserted);
    assert(solar_os_drv8830_drive("motor1", 480) == ESP_OK && regs[1][8][0] == 0x19);
    assert(solar_os_drv8830_drive("motor0", 2570) == ESP_OK && regs[0][0][0] == 0x81);
    assert(solar_os_drv8830_status("motor0", &status) == ESP_OK && status.chip.voltage_mv == 2570);
    history_len = 0;
    assert(solar_os_drv8830_drive("motor0", -5060) == ESP_OK && regs[0][0][0] == 0xfe);
    assert(history_len == 2 && history[0] == 0 && history[1] == 0xfe);
    assert(solar_os_drv8830_status("motor0", &status) == ESP_OK && status.chip.mode == SOLAR_OS_DRV8830_REVERSE);
    assert(status.chip.voltage_mv == 5060);
    const unsigned before = writes;
    assert(solar_os_drv8830_drive("motor0", 479) == ESP_ERR_INVALID_ARG);
    assert(solar_os_drv8830_drive("motor0", -479) == ESP_ERR_INVALID_ARG);
    assert(solar_os_drv8830_drive("motor0", 5061) == ESP_ERR_INVALID_ARG);
    assert(solar_os_drv8830_drive("motor0", INT_MIN) == ESP_ERR_INVALID_ARG && writes == before);

    /* All fault sources block new drive commands, with or without INT. */
    for (unsigned bit = 1; bit <= 0x10; bit <<= 1) {
        regs[1][8][1] = (uint8_t)bit;
        assert(solar_os_drv8830_drive("motor1", 2000) == ESP_ERR_INVALID_STATE);
        assert(solar_os_drv8830_status("motor1", &status) == ESP_OK && status.chip.fault == bit && !status.interrupt_present);
        assert(solar_os_drv8830_clear("motor1") == ESP_OK && regs[1][8][0] == 0 && regs[1][8][1] == 0);
    }
    line_level = false;
    assert(solar_os_drv8830_status("motor0", &status) == ESP_OK && status.fault_asserted);
    assert(solar_os_drv8830_drive("motor0", 2000) == ESP_ERR_INVALID_STATE);
    assert(solar_os_drv8830_drive("motor0", 0) == ESP_OK && regs[0][0][0] == 0);
    line_level = true;
    fail_line = true;
    assert(solar_os_drv8830_drive("motor0", 2000) == ESP_FAIL);
    assert(solar_os_drv8830_stop("motor0", false) == ESP_OK);
    fail_line = false;
    assert(solar_os_drv8830_stop("motor0", true) == ESP_OK && regs[0][0][0] == 0x1b);
    fail_read = 1;
    assert(solar_os_drv8830_drive("motor0", 2000) == ESP_FAIL && regs[0][0][0] == 0x1b);
    fail_read = -1;
    fail_write = 0; write_failures = 1;
    assert(solar_os_drv8830_drive("motor0", 2000) == ESP_FAIL && regs[0][0][0] == 0);
    assert(solar_os_drv8830_drive("motor0", 2000) == ESP_OK);
    write_failures = 2;
    assert(driver->detach("motor0") == ESP_FAIL && allocations == 2);
    assert(solar_os_drv8830_status("motor0", &status) == ESP_OK);
    fail_write = -1;
    speed = 1000000U;
    assert(solar_os_drv8830_status("motor0", &status) == ESP_ERR_INVALID_STATE);
    assert(driver->detach("motor0") == ESP_ERR_INVALID_STATE && allocations == 2);
    speed = 100000U;
    fail_reset = true;
    assert(driver->detach("motor0") == ESP_FAIL && allocations == 2);
    fail_reset = false;
    assert(driver->detach("motor0") == ESP_OK && allocations == 1 && regs[0][0][0] == 0);
    assert(driver->detach("motor1") == ESP_OK && allocations == 0);
    assert(driver->detach("motor1") == ESP_ERR_NOT_FOUND);
    assert(solar_os_drv8830_drive("motor0", 2000) == ESP_ERR_NOT_FOUND);
    assert(solar_os_drv8830_status(NULL, &status) == ESP_ERR_INVALID_ARG);
    assert(solar_os_drv8830_status("motor1", NULL) == ESP_ERR_INVALID_ARG);
    assert(!solar_os_drv8830_get(0, &info));
    puts("drv8830_test: PASS");
    return 0;
}
