#pragma once

#include <stdint.h>
#include "esp_err.h"

/* Runtime-only raw TCP listeners exposed as exclusive duplex byte streams.
 * Reads and writes are nonblocking. Release the stream before removing it. */
esp_err_t solar_os_tcp_stream_listen(const char *name, uint16_t port);
esp_err_t solar_os_tcp_stream_remove(const char *name);
