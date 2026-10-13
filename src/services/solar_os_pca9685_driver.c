#include "solar_os_pca9685.h"

static const int addresses[] = {
    0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47,
    0x48, 0x49, 0x4a, 0x4b, 0x4c, 0x4d, 0x4e, 0x4f,
    0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57,
    0x58, 0x59, 0x5a, 0x5b, 0x5c, 0x5d, 0x5e, 0x5f,
    0x60, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67,
    0x68, 0x69, 0x6a, 0x6b, 0x6c, 0x6d, 0x6e, 0x6f,
    0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77,
};
static const solar_os_expansion_binding_spec_t bindings[] = {
    {.key = "i2c", .value_hint = "bus", .kind = SOLAR_OS_EXPANSION_BINDING_I2C_BUS, .required = true},
    {.key = "addr", .value_hint = "0x40", .kind = SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS, .required = true,
        .allowed_values = addresses, .allowed_value_count = sizeof(addresses) / sizeof(addresses[0])},
    {.key = "oe", .value_hint = "gpio|controller:line", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO_LINE, .role = "oe"},
    {.key = "osc_hz", .value_hint = "25000000", .kind = SOLAR_OS_EXPANSION_BINDING_PARAMETER, .role = "osc_hz",
        .has_value_range = true, .min_value = 20000000, .max_value = 30000000},
};
const solar_os_expansion_driver_t solar_os_pca9685_expansion_driver = {
    .name = "pca9685", .category = SOLAR_OS_EXPANSION_CATEGORY_UTILITY,
    .summary = "NXP PCA9685 16-channel PWM controller",
    .required_capabilities = SOLAR_OS_BOARD_CAP_EXPANSION_I2C, .probe_supported = true,
    .binding_specs = bindings, .binding_spec_count = sizeof(bindings) / sizeof(bindings[0]),
    .attach = solar_os_pca9685_attach, .detach = solar_os_pca9685_detach,
};
