#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "i2c_bus.h"
#include "solar_os_buses.h"
#include "solar_os_display.h"
#include "solar_os_ssd1306.h"
#include "ssd1306.h"

typedef struct {
    unsigned probes, writes;
    size_t data_bytes;
    uint8_t last_command;
    uint8_t commands[SSD1306_I2C_TRANSFER_MAX];
    size_t command_bytes;
    const uint8_t *frame_bytes;
    unsigned page, x_offset;
    bool expect_data;
    esp_err_t probe_error, write_error;
} bus_t;
static bus_t buses[2];
static solar_os_display_target_t targets[2];
static esp_err_t register_error, unregister_error;
static unsigned legacy_calls;
static esp_err_t legacy_init_error = ESP_ERR_NOT_SUPPORTED;

size_t strlcpy(char *dst, const char *src, size_t size)
{
    const size_t len = strlen(src);
    if (size) { size_t n = len < size - 1U ? len : size - 1U; memcpy(dst, src, n); dst[n] = '\0'; }
    return len;
}

/* cam has a runtime bus, and no board-default I2C connection. */
esp_err_t i2c_bus_init(void) { legacy_calls++; return legacy_init_error; }
esp_err_t i2c_bus_probe(uint8_t address) { legacy_calls++; assert(address == 0x3c); return ESP_OK; }
esp_err_t i2c_bus_transmit(uint8_t address, const uint8_t *data, size_t len)
{ legacy_calls++; assert(address == 0x3c && data && len); return ESP_OK; }
void esp_rom_delay_us(uint32_t us) { (void)us; }
void vTaskDelay(TickType_t ticks) { assert(ticks > 0); }

static int bus_index(const char *name)
{
    return !strcmp(name, "i2c0") ? 0 : !strcmp(name, "aux") ? 1 : -1;
}

bool solar_os_expansion_find_i2c_bus(const char *name, solar_os_expansion_i2c_bus_t *bus, size_t *index)
{
    int i = bus_index(name);
    if (i < 0) return false;
    if (index) *index = (size_t)i;
    if (bus) memset(bus, 0, sizeof(*bus));
    return true;
}

esp_err_t solar_os_bus_i2c_probe(const char *name, uint8_t address)
{
    int i = bus_index(name); assert(i >= 0 && address == 0x3c);
    buses[i].probes++;
    return buses[i].probe_error;
}

esp_err_t solar_os_bus_i2c_transmit(const char *name, uint8_t address, const uint8_t *data, size_t len)
{
    int i = bus_index(name); assert(i >= 0 && address == 0x3c && len > 1 && len <= SSD1306_I2C_TRANSFER_MAX);
    bus_t *bus = &buses[i];
    bus->writes++;
    if (bus->write_error != ESP_OK) return bus->write_error;
    if (data[0] == 0x40) {
        bus->data_bytes += len - 1;
        if (bus->frame_bytes) {
            assert(bus->expect_data && len == 129 && bus->page < 8);
            assert(!memcmp(data + 1, bus->frame_bytes + bus->page * 128, 128));
            bus->page++;
            bus->expect_data = false;
        }
    } else {
        assert(data[0] == 0);
        bus->last_command = data[len - 1];
        bus->command_bytes = len - 1;
        memcpy(bus->commands, data + 1, len - 1);
        if (bus->frame_bytes) {
            const uint8_t expected[] = {0x40, 0x10, bus->x_offset, 0xb0 | bus->page};
            assert(!bus->expect_data && len == 5 && bus->page < 8);
            assert(!memcmp(data + 1, expected, sizeof(expected)));
            bus->expect_data = true;
        }
    }
    return ESP_OK;
}

static solar_os_display_target_t *target(const char *name)
{
    for (unsigned i = 0; i < 2; i++) if (!strcmp(targets[i].name, name)) return &targets[i];
    return NULL;
}

esp_err_t solar_os_display_register_target(const solar_os_display_target_t *value)
{
    if (register_error != ESP_OK) return register_error;
    assert(!target(value->name) && value->width == 128 && value->height == 64 && value->ready && value->u8g2);
    assert(value->preferred_stream_fps == 25);
    for (unsigned i = 0; i < 2; i++) if (!targets[i].name[0]) { targets[i] = *value; return ESP_OK; }
    return ESP_ERR_NO_MEM;
}

esp_err_t solar_os_display_unregister_target(const char *name)
{
    if (unregister_error != ESP_OK) return unregister_error;
    solar_os_display_target_t *value = target(name); assert(value);
    memset(value, 0, sizeof(*value));
    return ESP_OK;
}

static void bindings(solar_os_expansion_binding_t *result, const char *bus)
{
    result[0] = (solar_os_expansion_binding_t){.kind = SOLAR_OS_EXPANSION_BINDING_I2C_BUS};
    strlcpy(result[0].target, bus, sizeof(result[0].target));
    result[1] = (solar_os_expansion_binding_t){.kind = SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS, .value = 0x3c};
}

static void frame(const char *name, unsigned index)
{
    solar_os_display_target_t *value = target(name); assert(value);
    const unsigned other_writes = buses[index ^ 1U].writes;
    const unsigned prior_writes = buses[index].writes;
    const size_t prior = buses[index].data_bytes;
    uint8_t *pixels = u8g2_GetBufferPtr(value->u8g2);
    for (unsigned i = 0; i < SSD1306_BUFFER_SIZE; ++i) pixels[i] = (uint8_t)(i + i / 128 + prior_writes);
    buses[index].frame_bytes = pixels;
    buses[index].page = 0;
    buses[index].x_offset = !strcmp(value->driver, "sh1106") ? 2 : 0;
    buses[index].expect_data = false;
    u8g2_SendBuffer(value->u8g2);
    assert(buses[index].page == 8 && !buses[index].expect_data);
    buses[index].frame_bytes = NULL;
    assert(buses[index].writes == prior_writes + 16); /* Command group + data page, eight times. */
    assert(buses[index].data_bytes == prior + SSD1306_BUFFER_SIZE);
    assert(buses[index ^ 1U].writes == other_writes && legacy_calls == 0);
}

int main(void)
{
    solar_os_expansion_binding_t b[2]; bindings(b, "i2c0");
    assert(solar_os_sh1106_attach("oled0", b, 2) == ESP_OK);
    assert(!strcmp(target("oled0")->driver, "sh1106") && buses[0].writes > 0 && legacy_calls == 0);
    const unsigned first_writes = buses[0].writes;
    bindings(b, "aux");
    assert(solar_os_ssd1306_attach("oled1", b, 2) == ESP_OK);
    assert(!strcmp(target("oled1")->driver, "ssd1306") && buses[1].writes > 0 && buses[0].writes == first_writes);
    /* Two displays can use the same address on separate named buses. Both
     * retain their transport during later frames and power-state changes. */
    frame("oled0", 0); frame("oled1", 1); frame("oled0", 0);
    /* A pending command on one display must survive interleaved work on
     * another display without sharing CAD state. */
    u8x8_t *a = &target("oled0")->u8g2->u8x8;
    u8x8_t *c = &target("oled1")->u8g2->u8x8;
    const unsigned before = buses[0].writes;
    u8x8_cad_StartTransfer(a); u8x8_cad_SendCmd(a, 0x81);
    u8x8_cad_StartTransfer(c); u8x8_cad_SendCmd(c, 0x81);
    u8x8_cad_SendArg(a, 0x11); u8x8_cad_SendArg(c, 0x22);
    assert(buses[0].writes == before);
    u8x8_cad_EndTransfer(c); u8x8_cad_EndTransfer(a);
    assert(buses[0].command_bytes == 2 && buses[0].commands[0] == 0x81 && buses[0].commands[1] == 0x11);
    assert(buses[1].command_bytes == 2 && buses[1].commands[0] == 0x81 && buses[1].commands[1] == 0x22);
    /* Delays flush prior commands before waiting; command buffers can also
     * flush at capacity, and long data blocks split at the page boundary. */
    u8x8_cad_StartTransfer(a); u8x8_cad_SendCmd(a, 0xae);
    u8x8_gpio_Delay(a, U8X8_MSG_DELAY_MILLI, 1);
    assert(buses[0].writes == before + 2 && buses[0].last_command == 0xae);
    for (unsigned i = 0; i < 130; ++i) u8x8_cad_SendCmd(a, 0x40);
    const size_t prior_data = buses[0].data_bytes;
    uint8_t data[255] = {0};
    u8x8_cad_SendData(a, sizeof(data), data); u8x8_cad_EndTransfer(a);
    assert(buses[0].command_bytes == 2 && buses[0].data_bytes == prior_data + sizeof(data));
    unregister_error = ESP_ERR_INVALID_STATE;
    assert(solar_os_ssd1306_detach("oled0") == ESP_ERR_INVALID_STATE && target("oled0"));
    frame("oled0", 0);
    unregister_error = ESP_OK;
    unsigned other = buses[1].writes;
    assert(solar_os_ssd1306_detach("oled0") == ESP_OK && !target("oled0"));
    assert(buses[0].last_command == 0xae && buses[1].writes == other);
    frame("oled1", 1);
    assert(solar_os_ssd1306_detach("oled1") == ESP_OK && buses[1].last_command == 0xae && legacy_calls == 0);
    assert(solar_os_ssd1306_detach("missing") == ESP_ERR_NOT_FOUND);
    bindings(b, "missing");
    assert(solar_os_sh1106_attach("bad", b, 2) == ESP_ERR_INVALID_ARG && legacy_calls == 0);
    bindings(b, "i2c0"); b[1].value = 0x60;
    assert(solar_os_sh1106_attach("bad", b, 2) == ESP_ERR_INVALID_ARG && legacy_calls == 0);
    bindings(b, "i2c0");
    buses[0].probe_error = ESP_ERR_NOT_FOUND;
    assert(solar_os_sh1106_attach("oled0", b, 2) == ESP_ERR_INVALID_RESPONSE && !target("oled0"));
    buses[0].probe_error = ESP_ERR_TIMEOUT;
    assert(solar_os_sh1106_attach("oled0", b, 2) == ESP_ERR_TIMEOUT);
    buses[0].probe_error = ESP_ERR_NO_MEM;
    assert(solar_os_sh1106_attach("oled0", b, 2) == ESP_ERR_NO_MEM);
    buses[0].probe_error = ESP_OK;
    buses[0].write_error = ESP_ERR_TIMEOUT;
    assert(solar_os_sh1106_attach("oled0", b, 2) == ESP_ERR_TIMEOUT && !target("oled0"));
    buses[0].write_error = ESP_OK;
    register_error = ESP_ERR_NO_MEM;
    assert(solar_os_sh1106_attach("oled0", b, 2) == ESP_ERR_NO_MEM && !target("oled0"));
    assert(buses[0].last_command == 0xae); /* Successful init was powered down on registration failure. */
    register_error = ESP_OK;
    assert(solar_os_sh1106_attach("oled0", b, 2) == ESP_OK);
    frame("oled0", 0);
    assert(solar_os_ssd1306_detach("oled0") == ESP_OK && legacy_calls == 0);
    ssd1306_t legacy;
    assert(ssd1306_init(&legacy, 0x3c, SSD1306_CONTROLLER_SH1106) == ESP_ERR_NOT_SUPPORTED && legacy_calls == 1);
    legacy_init_error = ESP_OK;
    assert(ssd1306_init(&legacy, 0x3c, SSD1306_CONTROLLER_SSD1306) == ESP_OK && legacy.initialized);
    ssd1306_deinit(&legacy);
    puts("OLED: real SSD1306/SH1106 initialization, frames and shutdown on independent named buses; errors and retries PASS");
    return 0;
}
