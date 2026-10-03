#include "solar_os_cw2017.h"

#include "cw2017.h"

static const int addresses[] = {CW2017_I2C_ADDRESS};
static const solar_os_expansion_binding_spec_t binding_specs[] = {
    {.key = "i2c", .value_hint = "bus", .kind = SOLAR_OS_EXPANSION_BINDING_I2C_BUS, .required = true},
    {.key = "addr", .value_hint = "0x63", .kind = SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS, .required = true, .allowed_values = addresses, .allowed_value_count = sizeof(addresses) / sizeof(addresses[0])},
};

const solar_os_expansion_driver_t solar_os_cw2017_expansion_driver = {
    .name = "cw2017",
    .category = SOLAR_OS_EXPANSION_CATEGORY_POWER,
    .summary = "CW2017 fuel gauge",
    .required_capabilities = SOLAR_OS_BOARD_CAP_I2C,
    .binding_specs = binding_specs,
    .binding_spec_count = sizeof(binding_specs) / sizeof(binding_specs[0]),
    .attach = solar_os_cw2017_attach,
    .detach = solar_os_cw2017_detach,
};
