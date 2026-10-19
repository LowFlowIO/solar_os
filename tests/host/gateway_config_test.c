/* Real Messaging, Contacts and Gateway services, with an in-memory legacy NVS. */
#define main messaging_existing_tests_main
#include "messaging_service_test.c"
#undef main
#include "../../src/services/solar_os_chat.c"

static bool legacy_url = true, legacy_token = true, legacy_enabled = true, legacy_user = true;
static unsigned nvs_writes;
esp_err_t nvs_open(const char *name, nvs_open_mode_t mode, nvs_handle_t *handle)
{
    assert(strcmp(name, "chat") == 0);
    if (!legacy_url && !legacy_token && !legacy_enabled && !legacy_user) return ESP_ERR_NVS_NOT_FOUND;
    if (mode == NVS_READWRITE) nvs_writes++;
    *handle = 1; return ESP_OK;
}
esp_err_t nvs_get_str(nvs_handle_t handle, const char *key, char *value, size_t *size)
{
    (void)handle;
    const char *text = NULL;
    if (strcmp(key, "url") == 0 && legacy_url) text = "chats://example.org:7443";
    if (strcmp(key, "token") == 0 && legacy_token) text = "retained-token";
    if (strcmp(key, "user") == 0 && legacy_user) text = "obsolete-user";
    if (!text) return ESP_ERR_NVS_NOT_FOUND;
    size_t length = strlen(text) + 1;
    if (value && *size < length) return ESP_ERR_INVALID_SIZE;
    if (value) memcpy(value, text, length);
    *size = length; return ESP_OK;
}
esp_err_t nvs_get_u8(nvs_handle_t handle, const char *key, uint8_t *value)
{
    (void)handle;
    if (strcmp(key, "enabled") || !legacy_enabled) return ESP_ERR_NVS_NOT_FOUND;
    *value = 1; return ESP_OK;
}
esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key)
{
    (void)handle;
    if (!strcmp(key, "url")) legacy_url = false;
    if (!strcmp(key, "token")) legacy_token = false;
    if (!strcmp(key, "enabled")) legacy_enabled = false;
    if (!strcmp(key, "user")) legacy_user = false;
    return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t handle) { (void)handle; return ESP_OK; }
void nvs_close(nvs_handle_t handle) { (void)handle; }
void solar_os_identity_get_user(char *out, size_t size) { strlcpy(out, "user", size); }
void solar_os_identity_get_hostname(char *out, size_t size) { strlcpy(out, "device", size); }

static void reboot_gateway(void)
{
    free(chat.events); free(chat.commands);
    vSemaphoreDelete(chat.lock); vSemaphoreDelete(chat.edit_lock);
    memset(&chat, 0, sizeof(chat)); restart_messages();
    assert(solar_os_chat_init() == ESP_OK);
}

int main(void)
{
    assert(mkdtemp(test_root));
    /* A failed staged promotion must retain every legacy key. */
    fail_migration_promotion = true;
    assert(solar_os_chat_init() == ESP_OK);
    assert(legacy_url && legacy_token && legacy_enabled && legacy_user && nvs_writes == 0);
    reboot_gateway();
    assert(!legacy_url && !legacy_token && !legacy_enabled && !legacy_user && nvs_writes == 1);
    solar_os_chat_config_t config;
    assert(solar_os_chat_get_config(&config) == ESP_OK);
    assert(config.enabled && strcmp(config.url, "chats://example.org:7443") == 0);
    assert(strcmp(config.token, "retained-token") == 0);
    solar_os_chat_channel_t channels[32];
    assert(solar_os_chat_channel_snapshot(channels, 32) == 1 && channels[0].desired);
    /* Reopening a valid file must not allocate an empty NVS namespace. */
    reboot_gateway(); assert(nvs_writes == 1);
    /* Recover a power interruption between file commit and NVS cleanup. */
    legacy_url = legacy_token = legacy_enabled = legacy_user = true;
    reboot_gateway();
    assert(!legacy_url && !legacy_token && !legacy_enabled && !legacy_user && nvs_writes == 2);
    /* Failed saves leave the active connection unchanged. */
    fail_migration_promotion = true;
    assert(solar_os_chat_configure("chats://other.example:7443", "new-token") != ESP_OK);
    assert(solar_os_chat_get_config(&config) == ESP_OK && strcmp(config.token, "retained-token") == 0);
    assert(solar_os_chat_leave("general") == ESP_OK);
    reboot_gateway();
    assert(solar_os_chat_channel_snapshot(channels, 32) == 1 && !channels[0].desired);
    char path[160];
    const char *files[] = {".chat/profiles/001.bin", ".messages/messages.bin"};
    for (size_t i = 0; i < 2; i++) {
        assert(solar_os_storage_default_path(files[i], path, sizeof(path)) == ESP_OK);
        assert(unlink(path) == 0);
    }
    const char *directories[] = {".chat/profiles", ".chat", ".messages"};
    for (size_t i = 0; i < 3; i++) {
        assert(solar_os_storage_default_path(directories[i], path, sizeof(path)) == ESP_OK);
        assert(rmdir(path) == 0);
    }
    assert(rmdir(test_root) == 0);
    puts("gateway config migration tests: ok");
    return 0;
}
