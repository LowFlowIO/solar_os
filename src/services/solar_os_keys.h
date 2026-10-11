#pragma once

#include <stdbool.h>
#include "solar_os_key_codes.h"

bool solar_os_key_parse(const char *text, uint8_t *key);
const char *solar_os_key_name(uint8_t key);
