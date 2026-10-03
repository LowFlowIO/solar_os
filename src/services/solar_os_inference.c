#include "solar_os_inference_backend.h"

#include <string.h>
#include "esp_timer.h"
#include "solar_os_memory.h"
#include "solar_os_task.h"

#define INFERENCE_STACK_BYTES (12U * 1024U)
typedef struct {
    uint32_t id;
    solar_os_inference_backend_t *backend;
    solar_os_inference_model_info_t info;
} inference_model_t;
struct solar_os_inference {
    solar_os_inference_cancel_fn cancel;
    void *user;
    inference_model_t models[SOLAR_OS_INFERENCE_MODELS_MAX];
};
typedef struct {
    bool load;
    volatile bool done;
    bool cancel;
    int64_t deadline;
    const char *path;
    inference_model_t *model;
    const solar_os_inference_input_t *inputs;
    size_t count;
    solar_os_inference_result_t *result;
    esp_err_t error;
} inference_work_t;

static bool inference_busy;
static uint32_t next_id;
const char *solar_os_inference_mode_name(solar_os_inference_mode_t mode)
{
    switch (mode) {
    case SOLAR_OS_INFERENCE_SINGLE: return "single";
    case SOLAR_OS_INFERENCE_AUTO: return "auto";
    case SOLAR_OS_INFERENCE_DUAL: return "dual";
    default: return "unknown";
    }
}
esp_err_t solar_os_inference_mode_parse(const char *name, solar_os_inference_mode_t *mode)
{
    if (!name || !mode) return ESP_ERR_INVALID_ARG;
    for (unsigned i = 0; i <= SOLAR_OS_INFERENCE_DUAL; ++i) {
        if (!strcmp(name, solar_os_inference_mode_name(i))) { *mode = i; return ESP_OK; }
    }
    return ESP_ERR_INVALID_ARG;
}
static const char *dtype_names[] = {
    "int8", "uint8", "int16", "uint16", "int32", "uint32", "int64", "uint64",
    "float32", "float64", "float16", "bool",
};
static const uint8_t dtype_sizes[] = {1, 1, 2, 2, 4, 4, 8, 8, 4, 8, 2, 1};
const char *solar_os_tensor_dtype_name(solar_os_tensor_dtype_t dtype)
{
    return (unsigned)dtype < sizeof(dtype_sizes) ? dtype_names[dtype] : "unknown";
}
size_t solar_os_tensor_dtype_bytes(solar_os_tensor_dtype_t dtype)
{
    return (unsigned)dtype < sizeof(dtype_sizes) ? dtype_sizes[dtype] : 0;
}
esp_err_t solar_os_tensor_dtype_parse(const char *name, solar_os_tensor_dtype_t *dtype)
{
    if (!name || !dtype) return ESP_ERR_INVALID_ARG;
    for (size_t i = 0; i < sizeof(dtype_sizes); ++i)
        if (!strcmp(name, dtype_names[i])) { *dtype = (solar_os_tensor_dtype_t)i; return ESP_OK; }
    return ESP_ERR_NOT_SUPPORTED;
}
static bool admit(void)
{
    bool expected = false;
    return __atomic_compare_exchange_n(&inference_busy, &expected, true, false,
        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}
static void leave(void) { __atomic_store_n(&inference_busy, false, __ATOMIC_RELEASE); }
static inference_model_t *find(solar_os_inference_t *s, uint32_t id)
{
    if (s && id) for (size_t i = 0; i < SOLAR_OS_INFERENCE_MODELS_MAX; ++i)
        if (s->models[i].id == id) return &s->models[i];
    return NULL;
}
static void tensor_clear(solar_os_inference_tensor_t *tensor)
{
    solar_os_memory_free(tensor->exponents);
    memset(tensor, 0, sizeof(*tensor));
}
static void model_clear(inference_model_t *m)
{
    solar_os_inference_backend_close(m->backend);
    for (size_t i = 0; i < SOLAR_OS_INFERENCE_PORTS_MAX; ++i) {
        tensor_clear(&m->info.inputs[i]); tensor_clear(&m->info.outputs[i]);
    }
    memset(m, 0, sizeof(*m));
}
void solar_os_inference_result_free(solar_os_inference_result_t *r)
{
    if (!r) return;
    for (size_t i = 0; i < SOLAR_OS_INFERENCE_PORTS_MAX; ++i) {
        tensor_clear(&r->outputs[i].tensor);
        solar_os_memory_free(r->outputs[i].data);
    }
    solar_os_memory_free(r);
}
esp_err_t solar_os_inference_create(solar_os_inference_cancel_fn cancel, void *user,
                                   solar_os_inference_t **out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    *out = solar_os_memory_calloc(1, sizeof(**out),
        SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "inference.session");
    if (!*out) return ESP_ERR_NO_MEM;
    (*out)->cancel = cancel; (*out)->user = user;
    return ESP_OK;
}
void solar_os_inference_destroy(solar_os_inference_t *s)
{
    if (!s) return;
    /* Owner serializes calls. Another session may be running; model destruction
     * waits for native execution rather than deleting live backend state. */
    while (!admit()) vTaskDelay(1);
    for (size_t i = 0; i < SOLAR_OS_INFERENCE_MODELS_MAX; ++i) model_clear(&s->models[i]);
    solar_os_memory_free(s); leave();
}
esp_err_t solar_os_inference_info(solar_os_inference_t *s, uint32_t id,
    const solar_os_inference_model_info_t **out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    *out = NULL;
    inference_model_t *m = find(s, id);
    if (!m) return ESP_ERR_NOT_FOUND;
    *out = &m->info; return ESP_OK;
}
static bool progress(void *user)
{
    inference_work_t *w = user;
    return __atomic_load_n(&w->cancel, __ATOMIC_ACQUIRE) ||
        esp_timer_get_time() >= w->deadline;
}
static void worker(void *user)
{
    inference_work_t *w = user;
    w->error = w->load ? solar_os_inference_backend_load(w->path, progress, w,
        &w->model->backend, &w->model->info) :
        solar_os_inference_backend_run(w->model->backend, w->inputs, w->count,
            progress, w, w->result);
    /* Publishing completion is the worker's last access to shared work. The
     * owner deletes this task; it must not self-delete or touch w afterward. */
    __atomic_store_n(&w->done, true, __ATOMIC_RELEASE);
    vTaskSuspend(NULL);
}
static esp_err_t execute(solar_os_inference_t *s, inference_work_t *w, uint32_t timeout)
{
    if (!timeout || timeout > SOLAR_OS_INFERENCE_TIMEOUT_MAX_MS) return ESP_ERR_INVALID_ARG;
    if (s->cancel && s->cancel(s->user)) return ESP_ERR_TIMEOUT;
    w->deadline = esp_timer_get_time() + timeout * 1000LL;
    TaskHandle_t task = NULL;
    /* Loading can read flash-backed storage; the worker's stack must be internal. */
    if (solar_os_task_create_pinned_internal(worker, "inference", INFERENCE_STACK_BYTES,
        w, tskIDLE_PRIORITY + 1, &task, tskNO_AFFINITY,
        SOLAR_OS_TASK_ROLE_FOREGROUND) != pdPASS) return ESP_ERR_NO_MEM;
    bool cancelled = false;
    /* This worker suspends for owner deletion. The generic wait helper's reap
     * delay is for self-deleting tasks and adds latency to every operation. */
    while (!__atomic_load_n(&w->done, __ATOMIC_ACQUIRE)) {
        if ((s->cancel && s->cancel(s->user)) || esp_timer_get_time() >= w->deadline) {
            cancelled = true;
            __atomic_store_n(&w->cancel, true, __ATOMIC_RELEASE);
        }
        vTaskDelay(1);
    }
    solar_os_task_delete_internal(task);
    if ((s->cancel && s->cancel(s->user)) || progress(w)) cancelled = true;
    return cancelled ? ESP_ERR_TIMEOUT : w->error;
}
esp_err_t solar_os_inference_load(solar_os_inference_t *s, const char *path,
    uint32_t timeout, uint32_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    *out = 0;
    if (!s || !path || !*path) return ESP_ERR_INVALID_ARG;
    if (!admit()) return ESP_ERR_INVALID_STATE;
    inference_model_t *m = NULL;
    for (size_t i = 0; i < SOLAR_OS_INFERENCE_MODELS_MAX; ++i)
        if (!s->models[i].id) { m = &s->models[i]; break; }
    esp_err_t error = ESP_ERR_NO_MEM;
    if (!m) goto done;
    inference_work_t *w = solar_os_memory_calloc(1, sizeof(*w),
        SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "inference.work");
    if (!w) goto done;
    w->load = true; w->path = path; w->model = m;
    error = execute(s, w, timeout);
    solar_os_memory_free(w);
    if (error == ESP_OK) {
        uint32_t previous = __atomic_load_n(&next_id, __ATOMIC_RELAXED);
        while (previous < 0x1fffffffU &&
            !__atomic_compare_exchange_n(&next_id, &previous, previous + 1U,
                false, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
        if (previous == 0x1fffffffU) error = ESP_ERR_NO_MEM;
        else { m->id = previous + 1U; *out = m->id; }
    }
    if (error != ESP_OK) model_clear(m);
done:
    leave(); return error;
}
static bool matches(const solar_os_inference_tensor_t *a, const solar_os_inference_tensor_t *b)
{
    return a->dtype == b->dtype && a->rank == b->rank && a->bytes == b->bytes &&
        !memcmp(a->shape, b->shape, a->rank * sizeof(a->shape[0])) &&
        a->exponent_count == b->exponent_count &&
        (!a->exponent_count || (a->exponents && b->exponents &&
            !memcmp(a->exponents, b->exponents, a->exponent_count * sizeof(int32_t))));
}
esp_err_t solar_os_inference_run(solar_os_inference_t *s, uint32_t id,
    const solar_os_inference_input_t *inputs, size_t count, uint32_t timeout,
    solar_os_inference_result_t **out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    *out = NULL;
    inference_model_t *m = find(s, id);
    if (!m) return ESP_ERR_NOT_FOUND;
    if (!inputs || count != m->info.input_count) return ESP_ERR_INVALID_ARG;
    bool seen[SOLAR_OS_INFERENCE_PORTS_MAX] = {false};
    for (size_t i = 0; i < count; ++i) {
        if (!inputs[i].name || !inputs[i].data) return ESP_ERR_INVALID_ARG;
        size_t j = 0;
        while (j < count && strcmp(inputs[i].name, m->info.inputs[j].name)) ++j;
        if (j == count || seen[j]) return ESP_ERR_INVALID_ARG;
        seen[j] = true;
        if (inputs[i].bytes != m->info.inputs[j].bytes) return ESP_ERR_INVALID_SIZE;
        if (inputs[i].tensor && !matches(inputs[i].tensor, &m->info.inputs[j]))
            return ESP_ERR_INVALID_ARG;
    }
    if (!admit()) return ESP_ERR_INVALID_STATE;
    esp_err_t error = ESP_ERR_NO_MEM;
    inference_work_t *w = solar_os_memory_calloc(1, sizeof(*w),
        SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "inference.work");
    if (!w) goto done;
    w->result = solar_os_memory_calloc(1, sizeof(*w->result),
        SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "inference.result");
    if (!w->result) goto cleanup;
    w->model = m; w->inputs = inputs; w->count = count;
    error = execute(s, w, timeout);
    if (error == ESP_OK) { *out = w->result; w->result = NULL; }
    else solar_os_inference_backend_reset(m->backend);
cleanup:
    solar_os_inference_result_free(w->result);
    solar_os_memory_free(w);
done:
    leave(); return error;
}
esp_err_t solar_os_inference_reset(solar_os_inference_t *s, uint32_t id)
{
    inference_model_t *m = find(s, id);
    if (!m) return ESP_ERR_NOT_FOUND;
    if (!admit()) return ESP_ERR_INVALID_STATE;
    solar_os_inference_backend_reset(m->backend); leave(); return ESP_OK;
}
esp_err_t solar_os_inference_set_mode(solar_os_inference_t *s, uint32_t id,
                                    solar_os_inference_mode_t mode)
{
    if ((unsigned)mode > SOLAR_OS_INFERENCE_DUAL) return ESP_ERR_INVALID_ARG;
    inference_model_t *m = find(s, id);
    if (!m) return ESP_ERR_NOT_FOUND;
    if (!admit()) return ESP_ERR_INVALID_STATE;
    solar_os_inference_backend_set_mode(m->backend, mode);
    m->info.mode = mode;
    leave(); return ESP_OK;
}
esp_err_t solar_os_inference_close(solar_os_inference_t *s, uint32_t id)
{
    inference_model_t *m = find(s, id);
    if (!m) return ESP_ERR_NOT_FOUND;
    if (!admit()) return ESP_ERR_INVALID_STATE;
    model_clear(m); leave(); return ESP_OK;
}
