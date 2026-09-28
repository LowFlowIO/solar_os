#include "solar_os_shell_commands.h"

#include "solar_os_native.h"
#include "solar_os_module_packages.h"
#include "solar_os_shell.h"
#include "solar_os_shell_common.h"
#include "solar_os_storage.h"

static esp_err_t native_shell_write(const char *text, size_t text_len, void *user)
{
    solar_os_shell_io_t *io = (solar_os_shell_io_t *)user;
    if (io == NULL || text == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    return solar_os_shell_io_write_len(io, text, text_len);
}

static esp_err_t native_shell_run(solar_os_shell_io_t *io,
                                  const char *command,
                                  const char *path,
                                  int argc,
                                  char **argv,
                                  solar_os_native_run_result_t *result)
{
    const solar_os_native_run_options_t options = {
        .write = native_shell_write,
        .write_user = io,
    };
    char detail[128];
    const esp_err_t err = solar_os_native_run(path,
                                              argc,
                                              argv,
                                              &options,
                                              result,
                                              detail,
                                              sizeof(detail));
    if (err != ESP_OK) {
        solar_os_shell_diag_esp(io, command, err, detail, NULL);
    }
    return err;
}

void solar_os_shell_cmd_load(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *io = solar_os_shell_command_io(ctx);
    if (argc < 2) {
        solar_os_shell_diag_missing(io,
                                    "load",
                                    "ELF path",
                                    "load <file.elf> [args...]");
        return;
    }

    char path[SOLAR_OS_STORAGE_PATH_MAX];
    if (!solar_os_shell_resolve_path_for_command(ctx,
                                                 io,
                                                 "load",
                                                 argv[1],
                                                 path,
                                                 sizeof(path))) {
        return;
    }

    char *module_argv[16] = {0};
    const int module_argc = argc - 1;
    if ((size_t)module_argc > sizeof(module_argv) / sizeof(module_argv[0])) {
        solar_os_shell_diag_problem(io,
                                    "load",
                                    "too many module arguments",
                                    "load <file.elf> [args...]",
                                    NULL);
        return;
    }
    module_argv[0] = path;
    for (int i = 2; i < argc; i++) {
        module_argv[i - 1] = argv[i];
    }

    solar_os_native_run_result_t result;
    const esp_err_t err = native_shell_run(io,
                                           "load",
                                           path,
                                           module_argc,
                                           module_argv,
                                           &result);
    if (err != ESP_OK) {
        return;
    }

    solar_os_shell_io_printf(io,
                             "load: completed %s (%u bytes, %s, ABI %u)\n",
                             argv[1],
                             (unsigned)result.elf.file_size,
                             solar_os_native_elf_machine_name(result.elf.machine),
                             (unsigned)SOLAR_OS_NATIVE_ABI_VERSION);
}

bool solar_os_shell_try_native_module(solar_os_context_t *ctx,
                                      int argc,
                                      char **argv,
                                      bool *matched)
{
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    solar_os_storage_metadata_t metadata;

    if (matched != NULL) {
        *matched = false;
    }
    if (ctx == NULL || argc < 1 || argv == NULL || argv[0] == NULL ||
        solar_os_module_package_path(SOLAR_OS_MODULE_TYPE_APP,
                                     argv[0],
                                     path,
                                     sizeof(path)) != ESP_OK ||
        solar_os_storage_stat(path, &metadata) != ESP_OK ||
        metadata.type != SOLAR_OS_STORAGE_ENTRY_FILE) {
        return true;
    }

    if (matched != NULL) {
        *matched = true;
    }
    (void)native_shell_run(solar_os_shell_command_io(ctx),
                           argv[0],
                           path,
                           argc,
                           argv,
                           NULL);
    return true;
}
