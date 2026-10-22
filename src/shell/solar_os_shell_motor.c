#include "solar_os_shell_commands.h"
#include "solar_os_shell_common.h"
#include "solar_os_shell_io.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include "solar_os_drv8830.h"

static void usage(solar_os_shell_io_t *io)
{
    solar_os_shell_io_writeln(io, "usage: motor [list] | status <name> | drive <name> <signed-mV> | coast|brake|clear <name>");
}

void solar_os_shell_cmd_motor(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *io = solar_os_shell_command_io(ctx);
    if (argc == 1 || (argc == 2 && strcmp(argv[1], "list") == 0)) {
        solar_os_drv8830_info_t info;
        if (!solar_os_drv8830_get(0U, &info)) solar_os_shell_io_writeln(io, "no motors attached");
        for (size_t i = 0; solar_os_drv8830_get(i, &info); i++) {
            solar_os_shell_io_table_printf(io, "%s i2c=%s addr=0x%02x int=%d\r\n",
                info.name, info.bus, (unsigned)info.address, info.interrupt_gpio);
        }
        return;
    }
    esp_err_t ret;
    if (argc == 3 && strcmp(argv[1], "status") == 0) {
        solar_os_drv8830_status_t status;
        ret = solar_os_drv8830_status(argv[2], &status);
        if (ret == ESP_OK) {
            static const char *const modes[] = {"coast", "forward", "reverse", "brake"};
            solar_os_shell_io_printf(io,
                "%s mode=%s nominal-mV=%u control=0x%02x fault=0x%02x FAULT=%u OCP=%u UVLO=%u OTS=%u ILIMIT=%u int=%s\r\n",
                argv[2], modes[status.chip.mode], (unsigned)status.chip.voltage_mv,
                (unsigned)status.chip.control, (unsigned)status.chip.fault,
                !!(status.chip.fault & SOLAR_OS_DRV8830_FAULT), !!(status.chip.fault & SOLAR_OS_DRV8830_OCP),
                !!(status.chip.fault & SOLAR_OS_DRV8830_UVLO), !!(status.chip.fault & SOLAR_OS_DRV8830_OTS),
                !!(status.chip.fault & SOLAR_OS_DRV8830_ILIMIT),
                !status.interrupt_present ? "unwired" : status.fault_asserted ? "asserted" : "inactive");
        }
    } else if (argc == 4 && strcmp(argv[1], "drive") == 0) {
        char *end;
        errno = 0;
        const long value = strtol(argv[3], &end, 10);
        if (errno || end == argv[3] || *end != '\0' || value < -5060 || value > 5060 ||
            (value != 0 && value > -480 && value < 480)) { usage(io); return; }
        ret = solar_os_drv8830_drive(argv[2], (int)value);
    } else if (argc == 3 && (strcmp(argv[1], "coast") == 0 || strcmp(argv[1], "brake") == 0)) {
        ret = solar_os_drv8830_stop(argv[2], strcmp(argv[1], "brake") == 0);
    } else if (argc == 3 && strcmp(argv[1], "clear") == 0) {
        ret = solar_os_drv8830_clear(argv[2]);
    } else { usage(io); return; }
    if (ret != ESP_OK) solar_os_shell_io_printf(io, "motor: %s\r\n", solar_os_shell_error_text(ret));
}
