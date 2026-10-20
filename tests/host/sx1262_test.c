#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <freertos/semphr.h>

static SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    static StaticSemaphore_t mutex;
    return xSemaphoreCreateMutexStatic(&mutex);
}

#include "../../src/drivers/sx1262.c"

static int64_t now_us;
static int64_t sleep_us;
static bool asleep, stuck_busy, fail_wake;
static bool retained;
static unsigned transfers, wakes;
static uint8_t opcodes[256];

int64_t esp_timer_get_time(void) { return now_us; }
void esp_rom_delay_us(uint32_t us) { now_us += us; }
void vTaskDelay(TickType_t ticks) { now_us += (int64_t)ticks * 1000; }
int gpio_get_level(gpio_num_t pin) { assert(pin == 13); return asleep || stuck_busy; }
esp_err_t gpio_config(const gpio_config_t *config) { (void)config; return ESP_OK; }
esp_err_t gpio_set_level(gpio_num_t pin, uint32_t level)
{ (void)pin; (void)level; return ESP_OK; }

esp_err_t solar_os_bus_spi_transfer(const char *name, int cs, uint8_t mode,
    uint32_t hz, const uint8_t *tx, uint8_t *rx, size_t len)
{
    assert(strcmp(name, "spi0") == 0 && cs == 9 && mode == 0 && hz != 0);
    assert(transfers < sizeof(opcodes));
    opcodes[transfers++] = tx[0];
    if (rx != NULL) memset(rx, 0, len);
    if (tx[0] == SX1262_OP_GET_STATUS && (asleep || fail_wake)) {
        wakes++;
        assert(now_us - sleep_us >= 500);
        if (fail_wake) return ESP_FAIL;
        asleep = false;
        /* SDO is undefined during wake; the driver must ignore this byte. */
        if (rx != NULL) memset(rx, 0xff, len);
        return ESP_OK;
    }
    assert(!asleep && !stuck_busy);
    if (tx[0] == SX1262_OP_SET_SLEEP) {
        assert(len == 2);
        retained = (tx[1] & 0x04U) != 0U;
        asleep = true;
        sleep_us = now_us;
    }
    if (tx[0] == SX1262_OP_GET_IRQ_STATUS) {
        assert(len == 4);
        rx[3] = SX1262_IRQ_TX_DONE;
    }
    return ESP_OK;
}

static void sleep_radio(sx1262_t *dev)
{
    assert(sx1262_set_state(dev, SOLAR_OS_RADIO_STATE_SLEEP) == ESP_OK);
    assert(asleep && dev->state == SOLAR_OS_RADIO_STATE_SLEEP);
}

int main(void)
{
    sx1262_t dev = {.spi_bus = "spi0", .cs_pin = 9, .busy_pin = 13,
        .speed_hz = 2000000, .state = SOLAR_OS_RADIO_STATE_STANDBY,
        .config = {.frequency_hz = 868000000, .modulation = SOLAR_OS_RADIO_MODULATION_LORA,
            .preamble_len = 8, .spreading_factor = 7, .rx_bandwidth_hz = 125000,
            .coding_rate_denominator = 5, .sync_word_len = 1, .sync_word = {0x12},
            .tx_power_dbm = 14, .variable_length = true, .payload_length = 255}};
    sleep_radio(&dev);
    assert(sx1262_set_state(&dev, SOLAR_OS_RADIO_STATE_STANDBY) == ESP_OK);
    assert(!asleep && dev.state == SOLAR_OS_RADIO_STATE_STANDBY && wakes == 1);
    assert(retained && dev.config.frequency_hz == 868000000);

    sleep_radio(&dev);
    unsigned before = transfers;
    assert(sx1262_set_state(&dev, SOLAR_OS_RADIO_STATE_SLEEP) == ESP_OK);
    assert(transfers == before && asleep); /* Repeated sleep must not wake. */
    assert(sx1262_set_state(&dev, SOLAR_OS_RADIO_STATE_RX) == ESP_OK);
    assert(!asleep && dev.state == SOLAR_OS_RADIO_STATE_RX && retained);
    assert(opcodes[before] == SX1262_OP_GET_STATUS && opcodes[before + 1] == SX1262_OP_SET_RX);

    sleep_radio(&dev);
    fail_wake = true;
    assert(sx1262_set_state(&dev, SOLAR_OS_RADIO_STATE_STANDBY) == ESP_FAIL);
    assert(dev.state == SOLAR_OS_RADIO_STATE_SLEEP && asleep);
    fail_wake = false;
    stuck_busy = true;
    assert(sx1262_set_state(&dev, SOLAR_OS_RADIO_STATE_STANDBY) == ESP_ERR_TIMEOUT);
    assert(dev.state == SOLAR_OS_RADIO_STATE_SLEEP);
    stuck_busy = false;
    assert(sx1262_set_state(&dev, SOLAR_OS_RADIO_STATE_STANDBY) == ESP_OK);

    /* Packet I/O and configuration also wake through the common command path. */
    sleep_radio(&dev);
    solar_os_radio_packet_t packet = {.len = 1, .data = {0x42}};
    assert(sx1262_send(&dev, &packet, 100) == ESP_OK);
    assert(!asleep && dev.state == SOLAR_OS_RADIO_STATE_STANDBY && retained);
    sleep_radio(&dev);
    solar_os_radio_config_t config = dev.config;
    config.frequency_hz = 915000000;
    assert(sx1262_configure(&dev, &config) == ESP_OK);
    assert(!asleep && dev.state == SOLAR_OS_RADIO_STATE_STANDBY);
    assert(dev.config.frequency_hz == 915000000);
    sleep_radio(&dev);
    assert(sx1262_receive(&dev, &packet, 8) == ESP_ERR_TIMEOUT);
    assert(!asleep && dev.state == SOLAR_OS_RADIO_STATE_RX && retained);
    /* Ordinary BUSY waits still apply when the radio is already awake. */
    stuck_busy = true;
    before = transfers;
    assert(sx1262_set_state(&dev, SOLAR_OS_RADIO_STATE_STANDBY) == ESP_ERR_TIMEOUT);
    assert(transfers == before && dev.state == SOLAR_OS_RADIO_STATE_RX);
    puts("SX1262 sleep/wake, retained configuration, failure retry and packet I/O: ok");
    return 0;
}
