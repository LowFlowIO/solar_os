#pragma once
#include "../../freertos/FreeRTOS.h"
#define portMAX_DELAY UINT32_MAX
#undef pdMS_TO_TICKS
/* Default to the board's 1000 Hz; allow a coarse-tick reset regression check. */
#ifndef SOLAR_OS_EPD_TEST_TICK_MS
#define SOLAR_OS_EPD_TEST_TICK_MS 1U
#endif
#define pdMS_TO_TICKS(ms) ((TickType_t)((ms) / SOLAR_OS_EPD_TEST_TICK_MS))
