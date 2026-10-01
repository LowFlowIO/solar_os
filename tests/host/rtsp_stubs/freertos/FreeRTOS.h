#pragma once
#include <stdint.h>
typedef uint32_t TickType_t;
typedef int BaseType_t;
typedef unsigned UBaseType_t;
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#define pdPASS 1
#define tskIDLE_PRIORITY 0U
#define tskNO_AFFINITY -1
#define portMAX_DELAY UINT32_MAX
