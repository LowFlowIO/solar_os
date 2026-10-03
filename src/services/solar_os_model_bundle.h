#pragma once
#include "solar_os_inference_backend.h"
#include "solar_os_tensor_image.h"

#ifdef __cplusplus
extern "C" {
#endif
typedef struct solar_os_model_bundle solar_os_model_bundle_t;
/* Native bundle requests borrow inputs only until completion. An image view must
 * remain immutable/alive throughout the call. Raw tensors retain native axis meaning. */
typedef struct {
    solar_os_inference_input_t tensor;
    const solar_os_raster_image_pixels_t *image;
} solar_os_inference_value_t;

esp_err_t solar_os_inference_load_bundle(solar_os_inference_t *client,
    const char *resolved_manifest, uint32_t timeout_ms, uint32_t *handle);
esp_err_t solar_os_inference_run_bundle(solar_os_inference_t *client, uint32_t handle,
    const solar_os_inference_value_t *inputs, size_t count, uint32_t timeout_ms,
    solar_os_inference_result_t **result);

/* Service-internal helpers; called exclusively under inference admission. */
esp_err_t solar_os_model_bundle_load(const char *manifest,
    solar_os_inference_cancel_fn cancel, void *user,
    solar_os_model_bundle_t **bundle, solar_os_inference_backend_t **backend,
    solar_os_inference_model_info_t *info);
esp_err_t solar_os_model_bundle_run(const solar_os_model_bundle_t *bundle,
    const solar_os_inference_model_info_t *info, solar_os_inference_backend_t *backend,
    const solar_os_inference_value_t *inputs, size_t count,
    solar_os_inference_cancel_fn cancel, void *user, solar_os_inference_result_t *result);
void solar_os_model_bundle_free(solar_os_model_bundle_t *bundle);
#ifdef __cplusplus
}
#endif
