#pragma once
#include "epd_ultrachip.h"

/* Xteink 800x480 glass, 800x600 addressed gates, panel-programmed OTP LUTs.
 * The profile must be selected explicitly; the controller is never probed. */
#define EPD_UC8179_PANEL_XTEINK_800X480 1U
esp_err_t epd_uc8179_init(epd_ultrachip_t *display, const epd_ultrachip_config_t *config);
