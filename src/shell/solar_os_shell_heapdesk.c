#include "solar_os_shell_commands.h"

#include "solar_os_config.h"

#if SOLAR_OS_PACKAGE_SERVICE_HEAPDESK

#include <string.h>

#include "esp_err.h"
#include "heapdesk.h"
#include "solar_os_heapdesk.h"
#include "solar_os_shell_common.h"
#include "solar_os_shell_io.h"

static const char * const heapdesk_keys[] = {"mqtt_uri", "api_url", "device_id", "secret"};

static bool heapdesk_valid_key(const char *key)
{
    for (size_t i = 0; i < sizeof(heapdesk_keys) / sizeof(heapdesk_keys[0]); ++i) {
        if (strcmp(key, heapdesk_keys[i]) == 0) return true;
    }
    return false;
}

static void heapdesk_usage(solar_os_shell_io_t *io)
{
    solar_os_shell_io_writeln(io, "usage:");
    solar_os_shell_io_writeln(io, "  heapdesk status");
    solar_os_shell_io_writeln(io, "  heapdesk set <mqtt_uri|api_url|device_id|secret> <value>");
    solar_os_shell_io_writeln(io, "  heapdesk clear [key]");
    solar_os_shell_io_writeln(io, "  heapdesk start");
}

void solar_os_shell_cmd_heapdesk(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *io = solar_os_shell_command_io(ctx);
    if (argc < 2 || strcmp(argv[1], "status") == 0) {
        char value[160];
        for (size_t i = 0; i < sizeof(heapdesk_keys) / sizeof(heapdesk_keys[0]); ++i) {
            solar_os_heapdesk_get(heapdesk_keys[i], value, sizeof(value));
            const bool secret = strcmp(heapdesk_keys[i], "secret") == 0;
            solar_os_shell_io_printf(io, "%-10s %s\r\n", heapdesk_keys[i],
                                     value[0] == '\0' ? "(sdkconfig default)"
                                                      : (secret ? "(set)" : value));
        }
        solar_os_shell_io_printf(io, "connected  %s\r\n", heapdesk_connected() ? "yes" : "no");
        return;
    }
    if (strcmp(argv[1], "set") == 0 && argc == 4 && heapdesk_valid_key(argv[2])) {
        const esp_err_t err = solar_os_heapdesk_set(argv[2], argv[3]);
        solar_os_shell_io_printf(io, "%s: %s (takes effect on reboot or `heapdesk start`)\r\n",
                                 argv[2], esp_err_to_name(err));
        return;
    }
    if (strcmp(argv[1], "clear") == 0 && (argc == 2 || (argc == 3 && heapdesk_valid_key(argv[2])))) {
        const esp_err_t err = solar_os_heapdesk_erase(argc == 3 ? argv[2] : NULL);
        solar_os_shell_io_printf(io, "clear: %s\r\n", esp_err_to_name(err));
        return;
    }
    if (strcmp(argv[1], "start") == 0) {
        const esp_err_t err = solar_os_heapdesk_try_start();
        solar_os_shell_io_printf(io, "start: %s%s\r\n", esp_err_to_name(err),
                                 err == ESP_ERR_INVALID_STATE ? " (no IP or no device_id)" : "");
        return;
    }
    heapdesk_usage(io);
}

#endif
