#include "freertos/task.h"
#include "lc709203f.h"
#include "solar_os_battery.h"
#include "solar_os_buses.h"
#include "solar_os_lc709203f.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint16_t registers[256];
static unsigned reads, writes, delays;
static int corrupt = -1, fail_write = -1;
static int64_t now;
static solar_os_battery_config_t config;
static solar_os_battery_provider_t provider;
static bool fail_registration;

size_t strlcpy(char *dst, const char *src, size_t size) {
    size_t len = strlen(src);
    if (size != 0) {
        size_t n = len < size - 1 ? len : size - 1;
        memcpy(dst, src, n);
        dst[n] = 0;
    }
    return len;
}
int64_t esp_timer_get_time(void) { return now; }
void vTaskDelay(TickType_t ticks) { delays += ticks; }
bool solar_os_expansion_find_i2c_bus(const char *name,
                                     solar_os_expansion_i2c_bus_t *bus,
                                     size_t *index) {
    (void)bus;
    (void)index;
    return strcmp(name, "i2c0") == 0;
}
void solar_os_battery_get_config(solar_os_battery_config_t *value) {
    *value = config;
}
esp_err_t
solar_os_battery_register_provider(const char *name,
                                   const solar_os_battery_provider_t *value) {
    (void)name;
    if (fail_registration)
        return ESP_ERR_NO_MEM;
    provider = *value;
    return ESP_OK;
}
esp_err_t solar_os_battery_unregister_provider(const char *name) {
    (void)name;
    memset(&provider, 0, sizeof(provider));
    return ESP_OK;
}

static esp_err_t fake_read(void *user, uint8_t reg, uint8_t *data, size_t len) {
    (void)user;
    assert(len == 3);
    ++reads;
    data[0] = (uint8_t)registers[reg];
    data[1] = (uint8_t)(registers[reg] >> 8);
    const uint8_t packet[] = {0x16, reg, 0x17, data[0], data[1]};
    data[2] = solar_os_lc709203f_crc8(packet, 5);
    if (reg == corrupt)
        data[2] ^= 0x01;
    return ESP_OK;
}
static esp_err_t fake_write(void *user, uint8_t reg, const uint8_t *data,
                            size_t len) {
    (void)user;
    assert(len == 3);
    ++writes;
    if (reg == fail_write)
        return ESP_ERR_TIMEOUT;
    uint8_t packet[] = {0x16, reg, data[0], data[1]};
    assert(data[2] == solar_os_lc709203f_crc8(packet, 4));
    if (reg == 0x15 && data[0] == 1)
        assert(data[2] == 0x64); /* Independent wire-vector checksum. */
    registers[reg] = (uint16_t)(data[0] | ((uint16_t)data[1] << 8));
    if (reg == 0x12) {
        registers[0x0b] = 0;
        registers[0x15] = 2;
    }
    return ESP_OK;
}
static void fake_delay(void *user, unsigned ms) {
    (void)user;
    delays += ms;
}
esp_err_t solar_os_bus_i2c_read_reg(const char *bus, uint8_t address,
                                    uint8_t reg, uint8_t *data, size_t len) {
    assert(strcmp(bus, "i2c0") == 0 && address == 0x0b);
    return fake_read(NULL, reg, data, len);
}
esp_err_t solar_os_bus_i2c_write_reg(const char *bus, uint8_t address,
                                     uint8_t reg, const uint8_t *data,
                                     size_t len) {
    assert(strcmp(bus, "i2c0") == 0 && address == 0x0b);
    return fake_write(NULL, reg, data, len);
}

int main(void) {
    /* CRC vectors captured from the real Adafruit gauge, independent of this
     * driver. */
    const uint8_t version[] = {0x16, 0x11, 0x17, 0xff, 0x2a};
    const uint8_t voltage[] = {0x16, 0x09, 0x17, 0x45, 0x0f};
    const uint8_t soc[] = {0x16, 0x0f, 0x17, 0x5c, 0x02};
    assert(solar_os_lc709203f_crc8(version, 5) == 0xbd);
    assert(solar_os_lc709203f_crc8(voltage, 5) == 0x5c);
    assert(solar_os_lc709203f_crc8(soc, 5) == 0xe1);
    assert(solar_os_lc709203f_capacity_apa(3000) == 0x36);
    assert(solar_os_lc709203f_capacity_apa(1000) == 0x19);
    assert(solar_os_lc709203f_capacity_apa(100000) == 0x36);
    registers[0x11] = 0x2aff;
    registers[0x09] = 3909;
    registers[0x0f] = 604;
    const solar_os_lc709203f_io_t io = {
        .read = fake_read, .write = fake_write, .delay_ms = fake_delay};
    solar_os_lc709203f_t gauge;
    assert(solar_os_lc709203f_init(&gauge, &io, 0x36, 1, 250) == ESP_OK);
    assert(registers[0x15] == 1 && registers[0x16] == 0 &&
           registers[0x08] == 2982);
    assert(delays == 200 && registers[0x0b] == 0x36);
    solar_os_lc709203f_sample_t sample;
    assert(solar_os_lc709203f_read_sample(&gauge, &sample) == ESP_OK);
    assert(sample.voltage_mv == 3909 && sample.percent == 60 &&
           sample.ite_tenths == 604);
    registers[0x15] = 2;
    assert(solar_os_lc709203f_read_sample(&gauge, &sample) == ESP_OK);
    assert(registers[0x15] == 1);
    corrupt = 0x09;
    sample.voltage_mv = 1234;
    assert(solar_os_lc709203f_read_sample(&gauge, &sample) ==
           ESP_ERR_INVALID_CRC);
    assert(sample.voltage_mv == 1234);
    corrupt = -1;
    registers[0x0f] = 1001;
    assert(solar_os_lc709203f_read_sample(&gauge, &sample) ==
           ESP_ERR_INVALID_RESPONSE);
    registers[0x0f] = 604;
    fail_write = 0x0b;
    assert(solar_os_lc709203f_set_apa(&gauge, 0x19) == ESP_ERR_TIMEOUT &&
           gauge.apa == 0x36);
    assert(solar_os_lc709203f_init(&gauge, &io, 0x36, 1, 250) ==
               ESP_ERR_TIMEOUT &&
           !gauge.initialized);
    fail_write = -1;
    corrupt = 0x11;
    unsigned before = writes;
    assert(solar_os_lc709203f_init(&gauge, &io, 0x36, 1, 250) ==
               ESP_ERR_INVALID_CRC &&
           writes == before);
    corrupt = -1;

    solar_os_expansion_binding_t bindings[] = {
        {.kind = SOLAR_OS_EXPANSION_BINDING_I2C_BUS, .target = "i2c0"},
        {.kind = SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS, .value = 0x0b},
        {.kind = SOLAR_OS_EXPANSION_BINDING_PARAMETER,
         .role = "capacity",
         .value = 3000},
    };
    fail_registration = true;
    assert(solar_os_lc709203f_attach("battery0", bindings, 3) ==
           ESP_ERR_NO_MEM);
    fail_registration = false;
    assert(solar_os_lc709203f_attach("battery0", bindings, 3) == ESP_OK);
    solar_os_battery_sample_t value;
    assert(provider.read(provider.user, &value) == ESP_OK &&
           value.percent_valid && value.percent == 60);
    assert(!value.external_power_valid && !value.charging_valid);
    before = reads;
    assert(provider.read(provider.user, &value) == ESP_OK && reads == before);
    config.capacity_mah = 1000;
    assert(provider.read(provider.user, &value) == ESP_OK &&
           registers[0x0b] == 0x19 && reads > before);
    now = 3000000;
    corrupt = 0x0f;
    assert(provider.read(provider.user, &value) == ESP_ERR_INVALID_CRC);
    solar_os_battery_provider_t stale = provider;
    assert(solar_os_lc709203f_detach("battery0") == ESP_OK &&
           provider.read == NULL);
    assert(registers[0x15] == 1); /* Detach preserves autonomous tracking. */
    assert(stale.read(stale.user, &value) == ESP_ERR_INVALID_STATE);
    puts("LC709203F protocol and provider tests: ok");
}
