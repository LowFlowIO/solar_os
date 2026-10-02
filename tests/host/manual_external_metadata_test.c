#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "solar_os_docs.h"
#include "solar_os_manual.h"
#include "solar_os_memory.h"

static bool external_active = true;
static bool external_file_available = true;

static const solar_os_manual_page_t external_pages[] = {
    {
        .id = "overview",
        .title = "Downloaded overview",
        .section = "concept",
        .section_title = "Downloaded section",
        .summary = "Downloaded manual metadata",
        .aliases = "manual",
        .keywords = "downloaded external metadata",
        .body = "embedded body",
        .contract = "Downloaded overview reference",
    },
    {
        .id = "command.schedule",
        .title = "Downloaded schedule command",
        .section = "command",
        .section_title = "Commands",
        .summary = "Fresh schedule routing metadata",
        .aliases = "schedule",
        .keywords = "fresh scheduler command",
        .body = "embedded body",
        .contract = "Downloaded schedule reference",
    },
};

bool solar_os_docs_manual_index_available(void)
{
    return external_active;
}

size_t solar_os_docs_manual_count(void)
{
    return external_active ?
        sizeof(external_pages) / sizeof(external_pages[0]) : 0U;
}

const solar_os_manual_page_t *solar_os_docs_manual_get(size_t index)
{
    return external_active && index < solar_os_docs_manual_count() ?
        &external_pages[index] : NULL;
}

esp_err_t solar_os_docs_load_page(const char *id, char **body, size_t *body_len)
{
    if (!external_active || !external_file_available ||
        strcmp(id, "overview") != 0) {
        return ESP_ERR_NOT_FOUND;
    }
    const char *source = "+++\nid = \"overview\"\n+++\n"
                         "# Downloaded overview\n\n"
                         "The complete downloaded guide.\n\n"
                         "## Quick reference\n\n"
                         "The complete downloaded API contract.\n";
    *body_len = strlen(source);
    *body = malloc(*body_len + 1U);
    assert(*body != NULL);
    memcpy(*body, source, *body_len + 1U);
    return ESP_OK;
}

void *solar_os_memory_alloc(size_t size,
                            solar_os_memory_class_t memory_class,
                            const char *tag)
{
    (void)memory_class;
    (void)tag;
    return malloc(size);
}

void solar_os_memory_free(void *pointer)
{
    free(pointer);
}

int main(void)
{
    assert(solar_os_manual_count() == 2U);
    const solar_os_manual_page_t *page = solar_os_manual_find("schedule");
    assert(page == &external_pages[1]);
    assert(strcmp(page->summary, "Fresh schedule routing metadata") == 0);

    const solar_os_manual_page_t *matches[2] = {0};
    assert(solar_os_manual_search("fresh scheduler", matches, 2U) == 1U);
    assert(matches[0] == &external_pages[1]);
    assert(solar_os_manual_reference_count() == 0U);

    page = solar_os_manual_find("overview");
    const char *text = NULL;
    size_t text_len = 0U;
    bool owned = false;
    assert(solar_os_manual_load_body(page, &text, &text_len, &owned) == ESP_OK);
    assert(owned && strstr(text, "complete downloaded guide") != NULL);
    solar_os_manual_release_text(text, owned);
    assert(solar_os_manual_load_markdown(page, &text, &text_len, &owned) == ESP_OK);
    assert(owned && strstr(text, "# Downloaded overview") != NULL);
    assert(strstr(text, "+++") == NULL);
    solar_os_manual_release_text(text, owned);
    assert(solar_os_manual_load_contract(page, &text, &text_len, &owned) == ESP_OK);
    assert(owned && strcmp(text, "\nThe complete downloaded API contract.") == 0);
    solar_os_manual_release_text(text, owned);

    external_file_available = false;
    assert(solar_os_manual_load_body(page, &text, &text_len, &owned) == ESP_OK);
    assert(!owned && strcmp(text, "embedded body") == 0);

    external_active = false;
    assert(solar_os_manual_count() == solar_os_manual_embedded_count());
    assert(solar_os_manual_find("overview") == solar_os_manual_embedded_get(1U));
    assert(solar_os_manual_reference_count() == 0U);
    page = solar_os_manual_find("help");
    assert(solar_os_manual_load_body(page, &text, &text_len, &owned) == ESP_OK);
    assert(!owned && strstr(text, "wifi connect SSID PASSWORD") != NULL);
    assert(strstr(text, "disk mount") != NULL);
    assert(strstr(text, "help update") != NULL);
    page = solar_os_manual_find("python.network");
    assert(page != NULL);
    assert(solar_os_manual_load_body(page, &text, &text_len, &owned) == ESP_OK);
    assert(!owned && strstr(text, "downloadable manual") != NULL);
    assert(strstr(text, "post(url") == NULL);
    const char *body = text;
    assert(solar_os_manual_load_markdown(page, &text, &text_len, &owned) == ESP_OK);
    assert(!owned && text == body);
    return 0;
}
