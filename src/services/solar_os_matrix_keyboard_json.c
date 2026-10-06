#include "solar_os_matrix_keyboard_json.h"

#include <string.h>
#include "cJSON.h"

static bool number(const cJSON *item, unsigned max, unsigned *value)
{
    if (!cJSON_IsNumber(item) || !(item->valuedouble >= 0.0) ||
        !(item->valuedouble <= (double)max)) return false;
    *value = (unsigned)item->valuedouble;
    return (double)*value == item->valuedouble;
}

/* Reject excessive nesting before entering cJSON's recursive parser. The
 * schema only needs object -> array -> key object. Strings may contain braces. */
static bool bounded_depth(const char *json)
{
    unsigned depth = 0;
    bool string = false, escape = false;
    for (const char *p = json; *p != '\0'; p++) {
        if (string) {
            if (escape) escape = false;
            else if (*p == '\\') escape = true;
            else if (*p == '"') string = false;
        } else if (*p == '"') string = true;
        else if (*p == '{' || *p == '[') {
            if (++depth > 4U) return false;
        } else if (*p == '}' || *p == ']') {
            if (depth == 0U) return false;
            depth--;
        }
    }
    return depth == 0U && !string;
}

static bool known_fields(const cJSON *object, const char *const *names, unsigned count)
{
    unsigned seen = 0;
    const cJSON *item;
    cJSON_ArrayForEach(item, object) {
        if (item->string == NULL) return false;
        unsigned i = 0;
        while (i < count && strcmp(item->string, names[i]) != 0) i++;
        if (i == count || (seen & (1U << i)) != 0U) return false;
        seen |= 1U << i;
    }
    return true;
}

static bool optional_number(const cJSON *object, const char *name,
                             unsigned max, unsigned *value)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    *value = 0U;
    return item == NULL || number(item, max, value);
}

static bool optional_bool(const cJSON *object, const char *name, bool *value)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    *value = cJSON_IsTrue(item);
    return item == NULL || cJSON_IsBool(item);
}

static bool parse_layer(const cJSON *array, solar_os_matrix_keyboard_map_t *map,
                         unsigned layer)
{
    if (array == NULL) return true;
    if (!cJSON_IsArray(array) || cJSON_GetArraySize(array) > 80) return false;
    bool seen[SOLAR_OS_MATRIX_KEY_COUNT + 1U] = {false};
    static const char *const fields[] = {
        "row", "col", "usage", "key", "shift_key", "raw", "layer_tap", "alt_block",
    };
    const cJSON *entry;
    cJSON_ArrayForEach(entry, array) {
        unsigned row, col, usage, key, shift;
        bool raw, layer_tap, alt_block;
        if (!cJSON_IsObject(entry) || !known_fields(entry, fields, 8U) ||
            !number(cJSON_GetObjectItemCaseSensitive(entry, "row"), map->rows - 1U, &row) ||
            !number(cJSON_GetObjectItemCaseSensitive(entry, "col"), map->cols - 1U, &col) ||
            !optional_number(entry, "usage", 0xe7U, &usage) ||
            !optional_number(entry, "key", 255U, &key) ||
            !optional_number(entry, "shift_key", 255U, &shift) ||
            !optional_bool(entry, "raw", &raw) ||
            !optional_bool(entry, "layer_tap", &layer_tap) ||
            !optional_bool(entry, "alt_block", &alt_block) ||
            (unsigned)raw + (unsigned)layer_tap + (unsigned)alt_block > 1U)
            return false;
        unsigned id = 1U + row * 10U + col;
        if (seen[id]) return false;
        seen[id] = true;
        map->keys[layer][id] = (solar_os_matrix_key_t) {
            .usage = (uint16_t)usage, .key = (uint8_t)key, .shifted_key = (uint8_t)shift,
            .flags = raw ? SOLAR_OS_MATRIX_KEY_RAW :
                     layer_tap ? SOLAR_OS_MATRIX_KEY_LAYER_TAP :
                     alt_block ? SOLAR_OS_MATRIX_KEY_ALT_BLOCK : 0U,
        };
    }
    return true;
}

esp_err_t solar_os_matrix_keyboard_parse_map(const char *json,
    const solar_os_matrix_keyboard_map_t *base, solar_os_matrix_keyboard_map_t *out)
{
    if (json == NULL || out == NULL ||
        solar_os_matrix_keyboard_validate_map(base) != ESP_OK || !bounded_depth(json))
        return ESP_ERR_INVALID_ARG;
    cJSON *root = cJSON_ParseWithOpts(json, NULL, true);
    if (root == NULL) return ESP_ERR_INVALID_ARG;
    static const char *const fields[] = {"schema", "keys", "symbols"};
    unsigned schema = 0;
    solar_os_matrix_keyboard_map_t candidate = *base;
    bool valid = cJSON_IsObject(root) && known_fields(root, fields, 3U) &&
        number(cJSON_GetObjectItemCaseSensitive(root, "schema"), 1U, &schema) && schema == 1U &&
        (cJSON_HasObjectItem(root, "keys") || cJSON_HasObjectItem(root, "symbols")) &&
        parse_layer(cJSON_GetObjectItemCaseSensitive(root, "keys"), &candidate, 0U) &&
        parse_layer(cJSON_GetObjectItemCaseSensitive(root, "symbols"), &candidate, 1U) &&
        solar_os_matrix_keyboard_validate_map(&candidate) == ESP_OK;
    cJSON_Delete(root);
    if (!valid) return ESP_ERR_INVALID_ARG;
    *out = candidate;
    return ESP_OK;
}
