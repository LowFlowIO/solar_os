#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define RTC_PCF8563_ADDRESS 0x51U
#define RTC_PCF8563_BUS_NAME_MAX 16

typedef struct {
    char bus[RTC_PCF8563_BUS_NAME_MAX];
    uint8_t address;
    bool countdown_repeat;
    bool clock_integrity_lost;
} rtc_pcf8563_t;

typedef struct {
    uint16_t year;
    uint8_t month, day, weekday, hour, minute, second;
    bool clock_integrity;
} rtc_pcf8563_datetime_t;

typedef enum {
    RTC_PCF8563_ALARM_MATCH_SECOND = 1U << 0,
    RTC_PCF8563_ALARM_MATCH_MINUTE = 1U << 1,
    RTC_PCF8563_ALARM_MATCH_HOUR = 1U << 2,
    RTC_PCF8563_ALARM_MATCH_DAY = 1U << 3,
    RTC_PCF8563_ALARM_MATCH_WEEKDAY = 1U << 4,
} rtc_pcf8563_alarm_match_t;

typedef struct {
    uint32_t match_fields;
    uint8_t second, minute, hour, day, weekday;
} rtc_pcf8563_alarm_t;

typedef enum {
    RTC_PCF8563_INTERRUPT_ALARM = 1U << 0,
    RTC_PCF8563_INTERRUPT_COUNTDOWN = 1U << 1,
} rtc_pcf8563_interrupt_t;

esp_err_t rtc_pcf8563_init_device(rtc_pcf8563_t *device, const char *bus, uint8_t address);
esp_err_t rtc_pcf8563_get_datetime_device(const rtc_pcf8563_t *device,
                                          rtc_pcf8563_datetime_t *datetime);
esp_err_t rtc_pcf8563_set_datetime_device(rtc_pcf8563_t *device,
                                          const rtc_pcf8563_datetime_t *datetime);
esp_err_t rtc_pcf8563_set_alarm_device(const rtc_pcf8563_t *device,
                                       const rtc_pcf8563_alarm_t *alarm);
esp_err_t rtc_pcf8563_disable_alarm_device(const rtc_pcf8563_t *device);
esp_err_t rtc_pcf8563_set_countdown_device(rtc_pcf8563_t *device,
                                           uint32_t seconds, bool repeat);
esp_err_t rtc_pcf8563_disable_countdown_device(const rtc_pcf8563_t *device);
esp_err_t rtc_pcf8563_get_interrupt_status_device(const rtc_pcf8563_t *device,
                                                  uint32_t *interrupts);
esp_err_t rtc_pcf8563_clear_interrupt_status_device(const rtc_pcf8563_t *device,
                                                    uint32_t interrupts);
