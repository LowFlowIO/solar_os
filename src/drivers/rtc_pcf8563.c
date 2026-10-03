#include "rtc_pcf8563.h"

#include <string.h>
#include "solar_os_buses.h"

/* NXP PCF8563 register map; BM8563 uses the same calendar/alarm/timer map. */
#define CTRL1 0x00U
#define CTRL2 0x01U
#define SECONDS 0x02U
#define ALARM 0x09U
#define TIMER_CTRL 0x0eU
#define TIMER_VALUE 0x0fU
#define STOP 0x20U
#define AF 0x08U
#define TF 0x04U
#define AIE 0x02U
#define TIE 0x01U
#define TI_TP 0x10U
#define TE 0x80U
#define ALARM_FIELDS 0x1eU
#define INTERRUPTS (RTC_PCF8563_INTERRUPT_ALARM | RTC_PCF8563_INTERRUPT_COUNTDOWN)

static bool valid_device(const rtc_pcf8563_t *device)
{
    return device && device->bus[0] && device->address == RTC_PCF8563_ADDRESS;
}

static esp_err_t read_reg(const rtc_pcf8563_t *d, uint8_t reg, uint8_t *data, size_t len)
{
    return solar_os_bus_i2c_read_reg(d->bus, d->address, reg, data, len);
}

static esp_err_t write_reg(const rtc_pcf8563_t *d, uint8_t reg, const uint8_t *data, size_t len)
{
    return solar_os_bus_i2c_write_reg(d->bus, d->address, reg, data, len);
}

static esp_err_t update_control2(const rtc_pcf8563_t *d, uint8_t set, uint8_t clear)
{
    uint8_t value;
    esp_err_t err = read_reg(d, CTRL2, &value, 1);
    if (err != ESP_OK) return err;
    /* AF/TF are write-zero-to-clear. Write one to preserve an unrelated flag,
     * including an event arriving between the read and write. Reserved bits = 0. */
    value = (uint8_t)(((value & 0x1fU) | AF | TF | set) & ~clear);
    return write_reg(d, CTRL2, &value, 1);
}

static uint8_t bcd(uint8_t n) { return (uint8_t)((n / 10U) * 16U + n % 10U); }
static uint8_t decimal(uint8_t n) { return (uint8_t)((n >> 4) * 10U + (n & 15U)); }
static bool valid_bcd(uint8_t n) { return (n & 15U) <= 9 && (n >> 4) <= 9; }

static bool valid_datetime(const rtc_pcf8563_datetime_t *dt)
{
    static const uint8_t days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (!dt || dt->year < 2000 || dt->year > 2099 || dt->month < 1 || dt->month > 12 ||
        dt->weekday > 6 || dt->hour > 23 || dt->minute > 59 || dt->second > 59)
        return false;
    const unsigned max_day = days[dt->month - 1] +
        (dt->month == 2 && dt->year % 4 == 0 ? 1U : 0U);
    return dt->day >= 1 && dt->day <= max_day;
}

static uint8_t weekday(const rtc_pcf8563_datetime_t *dt)
{
    static const unsigned offsets[] = {0,3,2,5,0,3,5,1,4,6,2,4};
    unsigned year = dt->year - (dt->month < 3 ? 1U : 0U);
    return (uint8_t)((year + year/4 - year/100 + year/400 + offsets[dt->month-1] + dt->day) % 7);
}

esp_err_t rtc_pcf8563_init_device(rtc_pcf8563_t *device, const char *bus, uint8_t address)
{
    if (!device || !bus || !bus[0] || strnlen(bus, sizeof(device->bus)) >= sizeof(device->bus) ||
        address != RTC_PCF8563_ADDRESS) return ESP_ERR_INVALID_ARG;
    rtc_pcf8563_t candidate = {.address = address};
    strlcpy(candidate.bus, bus, sizeof(candidate.bus));
    uint8_t control;
    esp_err_t err = read_reg(&candidate, CTRL1, &control, 1);
    if (err != ESP_OK) return err;
    /* Start normal clock operation without clearing VL or calendar registers. */
    if ((control & 0xa8U) != 0) {
        candidate.clock_integrity_lost = true;
        control = 0;
        err = write_reg(&candidate, CTRL1, &control, 1);
        if (err != ESP_OK) return err;
    }
    *device = candidate;
    return ESP_OK;
}

esp_err_t rtc_pcf8563_get_datetime_device(const rtc_pcf8563_t *device,
                                          rtc_pcf8563_datetime_t *datetime)
{
    if (!valid_device(device) || !datetime) return ESP_ERR_INVALID_ARG;
    uint8_t data[9];
    esp_err_t err = read_reg(device, CTRL1, data, sizeof(data));
    if (err != ESP_OK) return err;
    const uint8_t values[] = {data[2] & 0x7fU, data[3] & 0x7fU, data[4] & 0x3fU,
        data[5] & 0x3fU, data[6] & 7U, data[7] & 0x1fU, data[8]};
    for (size_t i = 0; i < sizeof(values); ++i)
        if (!valid_bcd(values[i])) return ESP_ERR_INVALID_RESPONSE;
    /* Use C=0 for 2000..2099. C toggles on year rollover; the absolute century
     * convention is software-defined, so other conventions require setting time. */
    if ((data[7] & 0x80U) != 0) return ESP_ERR_NOT_SUPPORTED;
    const rtc_pcf8563_datetime_t value = {
        .year = (uint16_t)(2000 + decimal(values[6])),
        .month = decimal(values[5]), .weekday = decimal(values[4]),
        .day = decimal(values[3]), .hour = decimal(values[2]),
        .minute = decimal(values[1]), .second = decimal(values[0]),
        .clock_integrity = !device->clock_integrity_lost &&
            (data[2] & 0x80U) == 0 && (data[0] & 0xa8U) == 0,
    };
    if (!valid_datetime(&value)) return ESP_ERR_INVALID_RESPONSE;
    *datetime = value;
    return ESP_OK;
}

esp_err_t rtc_pcf8563_set_datetime_device(rtc_pcf8563_t *device,
                                          const rtc_pcf8563_datetime_t *datetime)
{
    if (!valid_device(device) || !valid_datetime(datetime)) return ESP_ERR_INVALID_ARG;
    uint8_t control;
    esp_err_t err = read_reg(device, CTRL1, &control, 1);
    if (err != ESP_OK) return err;
    const uint8_t stopped = STOP, running = 0;
    err = write_reg(device, CTRL1, &stopped, 1);
    if (err != ESP_OK) return err;
    device->clock_integrity_lost = true;
    const uint8_t data[] = {bcd(datetime->second), bcd(datetime->minute),
        bcd(datetime->hour), bcd(datetime->day), weekday(datetime),
        bcd(datetime->month), bcd((uint8_t)(datetime->year - 2000))};
    err = write_reg(device, SECONDS, data, sizeof(data));
    /* Always attempt to resume, even if the calendar transfer failed. */
    const esp_err_t resume = write_reg(device, CTRL1, &running, 1);
    if (err == ESP_OK && resume == ESP_OK) device->clock_integrity_lost = false;
    return err != ESP_OK ? err : resume;
}

esp_err_t rtc_pcf8563_set_alarm_device(const rtc_pcf8563_t *device,
                                       const rtc_pcf8563_alarm_t *alarm)
{
    if (!valid_device(device) || !alarm || !alarm->match_fields ||
        (alarm->match_fields & ~0x1fU) != 0) return ESP_ERR_INVALID_ARG;
    /* A zero second is compatible with minute-resolution alarms. Never silently
     * round a nonzero second or pretend there is a seconds comparator. */
    if ((alarm->match_fields & RTC_PCF8563_ALARM_MATCH_SECOND) != 0 &&
        (alarm->second != 0 || !(alarm->match_fields & RTC_PCF8563_ALARM_MATCH_MINUTE)))
        return ESP_ERR_NOT_SUPPORTED;
    const uint32_t fields = alarm->match_fields & ALARM_FIELDS;
    if (!fields ||
        ((fields & RTC_PCF8563_ALARM_MATCH_MINUTE) && alarm->minute > 59) ||
        ((fields & RTC_PCF8563_ALARM_MATCH_HOUR) && alarm->hour > 23) ||
        ((fields & RTC_PCF8563_ALARM_MATCH_DAY) && (alarm->day < 1 || alarm->day > 31)) ||
        ((fields & RTC_PCF8563_ALARM_MATCH_WEEKDAY) && alarm->weekday > 6))
        return ESP_ERR_INVALID_ARG;
    const uint8_t data[] = {
        fields & RTC_PCF8563_ALARM_MATCH_MINUTE ? bcd(alarm->minute) : 0x80U,
        fields & RTC_PCF8563_ALARM_MATCH_HOUR ? bcd(alarm->hour) : 0x80U,
        fields & RTC_PCF8563_ALARM_MATCH_DAY ? bcd(alarm->day) : 0x80U,
        fields & RTC_PCF8563_ALARM_MATCH_WEEKDAY ? alarm->weekday : 0x80U,
    };
    esp_err_t err = update_control2(device, 0, AIE);
    if (err != ESP_OK) return err;
    err = write_reg(device, ALARM, data, sizeof(data));
    return err == ESP_OK ? update_control2(device, AIE, AF) : err;
}

esp_err_t rtc_pcf8563_disable_alarm_device(const rtc_pcf8563_t *device)
{
    if (!valid_device(device)) return ESP_ERR_INVALID_ARG;
    esp_err_t err = update_control2(device, 0, AIE | AF);
    const uint8_t disabled[] = {0x80,0x80,0x80,0x80};
    return err == ESP_OK ? write_reg(device, ALARM, disabled, sizeof(disabled)) : err;
}

esp_err_t rtc_pcf8563_set_countdown_device(rtc_pcf8563_t *device,
                                           uint32_t seconds, bool repeat)
{
    if (!valid_device(device) || !seconds ||
        (seconds > 255 && (seconds > 15300 || seconds % 60 != 0)))
        return ESP_ERR_INVALID_ARG;
    const bool minutes = seconds > 255;
    const uint8_t disabled = 3, count = (uint8_t)(minutes ? seconds/60 : seconds);
    esp_err_t err = write_reg(device, TIMER_CTRL, &disabled, 1);
    if (err != ESP_OK) return err;
    err = update_control2(device, 0, TIE | TF | TI_TP);
    if (err != ESP_OK) return err;
    err = write_reg(device, TIMER_VALUE, &count, 1);
    if (err != ESP_OK) return err;
    err = update_control2(device, (uint8_t)(TIE | (repeat ? TI_TP : 0)), TF);
    if (err != ESP_OK) return err;
    const uint8_t enabled = (uint8_t)(TE | (minutes ? 3 : 2));
    err = write_reg(device, TIMER_CTRL, &enabled, 1);
    if (err == ESP_OK) device->countdown_repeat = repeat;
    return err;
}

esp_err_t rtc_pcf8563_disable_countdown_device(const rtc_pcf8563_t *device)
{
    if (!valid_device(device)) return ESP_ERR_INVALID_ARG;
    const uint8_t disabled = 3;
    esp_err_t err = write_reg(device, TIMER_CTRL, &disabled, 1);
    return err == ESP_OK ? update_control2(device, 0, TIE | TF | TI_TP) : err;
}

esp_err_t rtc_pcf8563_get_interrupt_status_device(const rtc_pcf8563_t *device,
                                                  uint32_t *interrupts)
{
    if (!valid_device(device) || !interrupts) return ESP_ERR_INVALID_ARG;
    uint8_t control;
    esp_err_t err = read_reg(device, CTRL2, &control, 1);
    if (err != ESP_OK) return err;
    if ((control & TF) && !device->countdown_repeat) {
        /* Hardware reloads automatically. Stop a one-shot on observing expiry;
         * leave TF latched until acknowledgement, including across sleep. */
        const uint8_t disabled = 3;
        err = write_reg(device, TIMER_CTRL, &disabled, 1);
        if (err != ESP_OK) return err;
    }
    *interrupts = ((control & AF) ? RTC_PCF8563_INTERRUPT_ALARM : 0) |
        ((control & TF) ? RTC_PCF8563_INTERRUPT_COUNTDOWN : 0);
    return ESP_OK;
}

esp_err_t rtc_pcf8563_clear_interrupt_status_device(const rtc_pcf8563_t *device,
                                                    uint32_t interrupts)
{
    if (!valid_device(device) || (interrupts & ~INTERRUPTS)) return ESP_ERR_INVALID_ARG;
    if ((interrupts & RTC_PCF8563_INTERRUPT_COUNTDOWN) && !device->countdown_repeat) {
        const uint8_t disabled = 3;
        esp_err_t err = write_reg(device, TIMER_CTRL, &disabled, 1);
        if (err != ESP_OK) return err;
    }
    const uint8_t clear = (uint8_t)(((interrupts & RTC_PCF8563_INTERRUPT_ALARM) ? AF : 0) |
        ((interrupts & RTC_PCF8563_INTERRUPT_COUNTDOWN) ? TF : 0));
    return update_control2(device, 0, clear);
}
