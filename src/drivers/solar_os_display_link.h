#pragma once
#include "esp_err.h"
#include "sdlp.h"
#include "solar_os_display_surface.h"
#include "u8g2.h"
typedef struct solar_os_display_link solar_os_display_link_t;
typedef struct {
  const char *bus;
  int cs, ready, reset;
  bool reset_active_high;
  uint32_t reset_ms, boot_ms, clock_hz;
} solar_os_display_link_config_t;
esp_err_t solar_os_display_link_create(const solar_os_display_link_config_t *,
                                       solar_os_display_link_t **);
void solar_os_display_link_destroy(solar_os_display_link_t *);
u8g2_t *solar_os_display_link_u8g2(solar_os_display_link_t *);
const sdlp_info_t *solar_os_display_link_info(const solar_os_display_link_t *);
esp_err_t solar_os_display_link_surface(void *,
                                        const solar_os_display_surface_t *);
esp_err_t solar_os_display_link_frame(void *,
                                      const solar_os_display_raster_t *);
esp_err_t solar_os_display_link_power_state(void *);
