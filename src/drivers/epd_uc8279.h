#pragma once
#include "epd_ultrachip.h"

/* Xteink 800x480 OTP/MTP-programmed glass: 600 addressed gates, visible 120..599.
 * X4 Pro programs PLL; X4 Classic retains the panel-programmed PLL setting.
 * The 792x528 X3 and other UC8279 variants require different panel profiles. */
#define EPD_UC8279_PANEL_X4_PRO_800X480 1U
#define EPD_UC8279_PANEL_X4_CLASSIC_800X480 2U
esp_err_t epd_uc8279_init(epd_ultrachip_t *display, const epd_ultrachip_config_t *config);
