#include "solar_os_qmi8658.h"

#include "qmi8658.h"

static const int addresses[] = {
    QMI8658_I2C_ADDRESS_LOW,
    QMI8658_I2C_ADDRESS_HIGH,
};

static const solar_os_expansion_binding_spec_t binding_specs[] = {
    {.key = "i2c", .value_hint = "bus", .kind = SOLAR_OS_EXPANSION_BINDING_I2C_BUS, .required = true},
    {.key = "addr", .value_hint = "0x6a|0x6b", .kind = SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS, .required = true, .allowed_values = addresses, .allowed_value_count = sizeof(addresses) / sizeof(addresses[0])},
};

const solar_os_expansion_driver_t solar_os_qmi8658_expansion_driver = {
    .name = "qmi8658",
    .category = SOLAR_OS_EXPANSION_CATEGORY_SENSOR,
    .summary = "QMI8658 six-axis IMU",
    .required_capabilities = SOLAR_OS_BOARD_CAP_I2C,
    .probe_supported = true,
    .binding_specs = binding_specs,
    .binding_spec_count = sizeof(binding_specs) / sizeof(binding_specs[0]),
    .attach = solar_os_qmi8658_attach,
    .detach = solar_os_qmi8658_detach,
};
