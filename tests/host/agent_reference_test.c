#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "solar_os_agent_reference.h"
#include "solar_os_manual.h"

#define RESULT_CAPACITY 4096U

void solar_os_memory_free(void *ptr)
{
    free(ptr);
}

static void expect_download_notice(const char *query, const char *unavailable_api)
{
    char result[RESULT_CAPACITY];
    assert(solar_os_agent_reference_search(query,
                                           result,
                                           sizeof(result)) == ESP_OK);
    assert(strlen(result) < sizeof(result));
    assert(strstr(result, "help update") != NULL);
    assert(strstr(result, "downloadable manual") != NULL);
    assert(strstr(result, unavailable_api) == NULL);
}

int main(void)
{
    assert(solar_os_manual_reference_count() == 0U);
    expect_download_notice("python http post response fields", "post(url");
    expect_download_notice("lua websocket ownership limits", "websocket_connect");
    expect_download_notice("python open external imports mpy", "open(path");
    expect_download_notice("lua gpio configure pull constants", "PULL_UP");

    char result[RESULT_CAPACITY];
    assert(solar_os_agent_reference_search("command.disk", result,
                                           sizeof(result)) == ESP_OK);
    assert(strstr(result, "disk lsblk") != NULL);
    assert(strstr(result, "disk mount") != NULL);
    assert(solar_os_agent_reference_search("help", result,
                                           sizeof(result)) == ESP_OK);
    assert(strstr(result, "help update") != NULL);
    return 0;
}
