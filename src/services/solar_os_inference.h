#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SOLAR_OS_INFERENCE_MODELS_MAX 4U
#define SOLAR_OS_INFERENCE_PORTS_MAX 16U
#define SOLAR_OS_INFERENCE_RANK_MAX 8U
#define SOLAR_OS_INFERENCE_NAME_MAX 128U
#define SOLAR_OS_INFERENCE_TENSOR_MAX (4U * 1024U * 1024U)
#define SOLAR_OS_INFERENCE_FILE_MAX (16U * 1024U * 1024U)
#define SOLAR_OS_INFERENCE_TIMEOUT_MAX_MS 60000U

typedef struct solar_os_inference solar_os_inference_t;
typedef bool (*solar_os_inference_cancel_fn)(void *user);
typedef enum {
    SOLAR_OS_INFERENCE_SINGLE, SOLAR_OS_INFERENCE_AUTO, SOLAR_OS_INFERENCE_DUAL,
} solar_os_inference_mode_t;
typedef enum {
    SOLAR_OS_TENSOR_INT8, SOLAR_OS_TENSOR_UINT8,
    SOLAR_OS_TENSOR_INT16, SOLAR_OS_TENSOR_UINT16,
    SOLAR_OS_TENSOR_INT32, SOLAR_OS_TENSOR_UINT32,
    SOLAR_OS_TENSOR_INT64, SOLAR_OS_TENSOR_UINT64,
    SOLAR_OS_TENSOR_FLOAT32, SOLAR_OS_TENSOR_FLOAT64,
    SOLAR_OS_TENSOR_FLOAT16, SOLAR_OS_TENSOR_BOOL,
} solar_os_tensor_dtype_t;

/* Dense tensors in the model's native axis order. Shape has no implicit image
 * meaning. Numeric buffers use little-endian elements. Exponents describe
 * ESP-DL power-of-two quantization: real = stored * 2**exponent; zero point 0.
 * A per-channel vector is exposed verbatim; no axis meaning is guessed. */
typedef struct {
    char name[SOLAR_OS_INFERENCE_NAME_MAX];
    solar_os_tensor_dtype_t dtype;
    uint32_t rank, shape[SOLAR_OS_INFERENCE_RANK_MAX];
    size_t bytes;
    size_t exponent_count;
    int32_t *exponents;
} solar_os_inference_tensor_t;

typedef struct {
    size_t input_count, output_count, model_bytes, internal_bytes, external_bytes;
    solar_os_inference_mode_t mode;
    solar_os_inference_tensor_t inputs[SOLAR_OS_INFERENCE_PORTS_MAX];
    solar_os_inference_tensor_t outputs[SOLAR_OS_INFERENCE_PORTS_MAX];
} solar_os_inference_model_info_t;

typedef struct {
    const char *name;
    const void *data;
    size_t bytes;
    /* Optional typed contract; NULL interprets raw bytes using the model port.
     * When present, shape/dtype/bytes and quantization must match exactly. */
    const solar_os_inference_tensor_t *tensor;
} solar_os_inference_input_t;

typedef struct {
    size_t count;
    uint64_t input_us, inference_us, output_us, elapsed_us;
    struct {
        solar_os_inference_tensor_t tensor;
        void *data;
    } outputs[SOLAR_OS_INFERENCE_PORTS_MAX];
} solar_os_inference_result_t;

const char *solar_os_tensor_dtype_name(solar_os_tensor_dtype_t dtype);
esp_err_t solar_os_tensor_dtype_parse(const char *name, solar_os_tensor_dtype_t *dtype);
size_t solar_os_tensor_dtype_bytes(solar_os_tensor_dtype_t dtype);
const char *solar_os_inference_mode_name(solar_os_inference_mode_t mode);
esp_err_t solar_os_inference_mode_parse(const char *name, solar_os_inference_mode_t *mode);

/* A session owns resident models. Handles never recycle, including across
 * sessions. One native operation globally at a time; competing work is busy.
 * Calls on a session must be serialized by its owner. Cancellation/deadlines
 * join the worker before returning; they are cooperative, not hard preemption.
 * Loading takes a resolved SolarOS storage path, with no zoo/catalog required. */
esp_err_t solar_os_inference_create(solar_os_inference_cancel_fn cancel, void *user,
                                   solar_os_inference_t **session);
void solar_os_inference_destroy(solar_os_inference_t *session);
esp_err_t solar_os_inference_load(solar_os_inference_t *session, const char *path,
                                 uint32_t timeout_ms, uint32_t *handle);
/* Borrowed immutable descriptors, valid until close/destroy. */
esp_err_t solar_os_inference_info(solar_os_inference_t *session, uint32_t handle,
                                 const solar_os_inference_model_info_t **info);
esp_err_t solar_os_inference_run(solar_os_inference_t *session, uint32_t handle,
    const solar_os_inference_input_t *inputs, size_t count, uint32_t timeout_ms,
    solar_os_inference_result_t **result);
esp_err_t solar_os_inference_reset(solar_os_inference_t *session, uint32_t handle);
/* Select execution mode without reloading the model. Default is single. */
esp_err_t solar_os_inference_set_mode(solar_os_inference_t *session, uint32_t handle,
                                    solar_os_inference_mode_t mode);
esp_err_t solar_os_inference_close(solar_os_inference_t *session, uint32_t handle);
void solar_os_inference_result_free(solar_os_inference_result_t *result);

#ifdef __cplusplus
}
#endif
