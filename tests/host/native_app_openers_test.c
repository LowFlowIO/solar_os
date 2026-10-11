#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include "solar_os_app_file_types.h"

typedef struct { const void *app; const char *file_extensions; } solar_os_app_registry_entry_t;
static int app;
static const solar_os_app_registry_entry_t entries[] = {
    {&app, ".txt"}, {&app, ".gb .bin"}, {&app, ""}, {NULL, ".bad"}
};
static bool installed;
static size_t solar_os_app_registry_count(void) { return installed ? 5U : 1U; }
static const solar_os_app_registry_entry_t *solar_os_app_registry_get(size_t i) {
    return i < 4U ? &entries[i] : NULL;
}
#include "app_opener.inc"

int main(void) {
    assert(solar_os_app_registry_find_opener(NULL) == NULL);
    assert(solar_os_app_registry_find_opener("notes.txt") == &entries[0]);
    assert(solar_os_app_registry_find_opener("roms/game.gb") == NULL);
    installed = true;
    assert(solar_os_app_registry_find_opener("roms/game.gb") == &entries[1]);
    assert(solar_os_app_registry_find_opener("roms/GAME.GB") == &entries[1]);
    assert(solar_os_app_registry_find_opener("game.bin") == &entries[1]);
    assert(solar_os_app_registry_find_opener("game.bad") == NULL);
    assert(solar_os_app_registry_find_opener("game.gbc") == NULL);
    assert(solar_os_app_registry_find_opener("roms/.gb") == NULL);
    installed = false;
    assert(solar_os_app_registry_find_opener("roms/game.gb") == NULL);
    puts("Installed foreground app file associations: OK");
    return 0;
}
