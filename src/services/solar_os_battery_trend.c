#include "solar_os_battery_trend.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define BATTERY_TREND_MIN_SPAN_MS (5U * 60U * 1000U)
#define BATTERY_TREND_BASE_INTERVALS 7U
#define BATTERY_TREND_MIN_SAMPLES 4U
#define BATTERY_TREND_SLOPE_THRESHOLD_MVH 40
#define BATTERY_TREND_DELTA_THRESHOLD_MV 5
#define BATTERY_TREND_ENDPOINT_SAMPLES 3U

void solar_os_battery_trend_init(
    solar_os_battery_trend_estimator_t *estimator,
    uint32_t interval_ms)
{
    if (estimator == NULL) {
        return;
    }

    memset(estimator, 0, sizeof(*estimator));
    estimator->interval_ms = interval_ms;
}

void solar_os_battery_trend_push(
    solar_os_battery_trend_estimator_t *estimator,
    uint32_t tick_ms,
    uint16_t voltage_mv)
{
    if (estimator == NULL) {
        return;
    }

    if (estimator->sample_count < SOLAR_OS_BATTERY_TREND_HISTORY) {
        estimator->samples[estimator->sample_count++] =
            (solar_os_battery_trend_sample_t) {
                .tick_ms = tick_ms,
                .voltage_mv = voltage_mv,
            };
        return;
    }

    memmove(&estimator->samples[0],
            &estimator->samples[1],
            sizeof(estimator->samples[0]) *
                (SOLAR_OS_BATTERY_TREND_HISTORY - 1U));
    estimator->samples[SOLAR_OS_BATTERY_TREND_HISTORY - 1U] =
        (solar_os_battery_trend_sample_t) {
            .tick_ms = tick_ms,
            .voltage_mv = voltage_mv,
        };
}

static uint32_t battery_trend_target_span_ms(uint32_t interval_ms)
{
    const uint64_t interval_span =
        (uint64_t)interval_ms * BATTERY_TREND_BASE_INTERVALS;
    uint64_t target = interval_span > BATTERY_TREND_MIN_SPAN_MS ?
        interval_span : BATTERY_TREND_MIN_SPAN_MS;
    if (target > UINT32_MAX / 2U) {
        target = UINT32_MAX / 2U;
    }
    return (uint32_t)target;
}

static bool battery_trend_select_window(
    const solar_os_battery_trend_estimator_t *estimator,
    size_t *start,
    uint32_t *span_ms)
{
    if (estimator->sample_count < BATTERY_TREND_MIN_SAMPLES) {
        return false;
    }

    const size_t newest = estimator->sample_count - 1U;
    const uint32_t target_span =
        battery_trend_target_span_ms(estimator->interval_ms);
    size_t selected = newest;
    uint32_t selected_span = 0;
    while (selected > 0U) {
        selected--;
        selected_span = estimator->samples[newest].tick_ms -
            estimator->samples[selected].tick_ms;
        if (selected_span >= target_span &&
            newest - selected + 1U >= BATTERY_TREND_MIN_SAMPLES) {
            break;
        }
    }

    if (selected_span < target_span ||
        newest - selected + 1U < BATTERY_TREND_MIN_SAMPLES) {
        return false;
    }

    *start = selected;
    *span_ms = selected_span;
    return true;
}

static bool battery_trend_calculate_slope(
    const solar_os_battery_trend_estimator_t *estimator,
    size_t start,
    int32_t *slope_mvh)
{
    const size_t count = estimator->sample_count - start;
    const uint32_t base_ms = estimator->samples[start].tick_ms;
    int64_t sum_t = 0;
    int64_t sum_v = 0;
    int64_t sum_tt = 0;
    int64_t sum_tv = 0;

    for (size_t i = start; i < estimator->sample_count; i++) {
        const int64_t t =
            (int64_t)((estimator->samples[i].tick_ms - base_ms) / 1000U);
        const int64_t v = estimator->samples[i].voltage_mv;
        sum_t += t;
        sum_v += v;
        sum_tt += t * t;
        sum_tv += t * v;
    }

    const int64_t n = (int64_t)count;
    const int64_t denominator = (n * sum_tt) - (sum_t * sum_t);
    if (denominator == 0) {
        return false;
    }

    const int64_t numerator = (n * sum_tv) - (sum_t * sum_v);
    *slope_mvh = (int32_t)((numerator * 3600LL) / denominator);
    return true;
}

static int32_t battery_trend_endpoint_delta(
    const solar_os_battery_trend_estimator_t *estimator,
    size_t start)
{
    const size_t count = estimator->sample_count - start;
    const size_t endpoint_count = count >= BATTERY_TREND_ENDPOINT_SAMPLES * 2U ?
        BATTERY_TREND_ENDPOINT_SAMPLES : 1U;
    uint32_t first_sum = 0;
    uint32_t last_sum = 0;
    for (size_t i = 0; i < endpoint_count; i++) {
        first_sum += estimator->samples[start + i].voltage_mv;
        last_sum += estimator->samples[
            estimator->sample_count - endpoint_count + i].voltage_mv;
    }

    const int32_t first = (int32_t)((first_sum + endpoint_count / 2U) /
                                    endpoint_count);
    const int32_t last = (int32_t)((last_sum + endpoint_count / 2U) /
                                   endpoint_count);
    return last - first;
}

bool solar_os_battery_trend_estimate(
    const solar_os_battery_trend_estimator_t *estimator,
    solar_os_battery_trend_result_t *result)
{
    if (estimator == NULL || result == NULL) {
        return false;
    }

    memset(result, 0, sizeof(*result));
    result->trend = SOLAR_OS_BATTERY_VOLTAGE_TREND_UNKNOWN;

    size_t start = 0;
    if (!battery_trend_select_window(estimator, &start, &result->span_ms) ||
        !battery_trend_calculate_slope(estimator,
                                       start,
                                       &result->slope_mvh)) {
        return false;
    }

    result->sample_count = estimator->sample_count - start;
    result->delta_mv = battery_trend_endpoint_delta(estimator, start);
    if (result->slope_mvh >= BATTERY_TREND_SLOPE_THRESHOLD_MVH &&
        result->delta_mv >= BATTERY_TREND_DELTA_THRESHOLD_MV) {
        result->trend = SOLAR_OS_BATTERY_VOLTAGE_TREND_RISING;
    } else if (result->slope_mvh <= -BATTERY_TREND_SLOPE_THRESHOLD_MVH &&
               result->delta_mv <= -BATTERY_TREND_DELTA_THRESHOLD_MV) {
        result->trend = SOLAR_OS_BATTERY_VOLTAGE_TREND_FALLING;
    } else {
        result->trend = SOLAR_OS_BATTERY_VOLTAGE_TREND_STABLE;
    }
    return true;
}
