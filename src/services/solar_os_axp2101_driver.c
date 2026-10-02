#include "solar_os_axp2101.h"

#include "axp2101.h"

static const int addresses[] = {AXP2101_I2C_ADDRESS};

static const solar_os_expansion_binding_spec_t binding_specs[] = {
    {.key = "i2c", .value_hint = "bus", .kind = SOLAR_OS_EXPANSION_BINDING_I2C_BUS, .required = true},
    {.key = "addr", .value_hint = "0x34", .kind = SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS, .required = true, .allowed_values = addresses, .allowed_value_count = sizeof(addresses) / sizeof(addresses[0])},
    {.key = "input_current", .value_hint = "100|500|900|1000|1500|2000mA", .kind = SOLAR_OS_EXPANSION_BINDING_PARAMETER, .role = "input_current", .has_value_range = true, .min_value = 100, .max_value = 2000},
    {.key = "charge_current", .value_hint = "100..1000mA", .kind = SOLAR_OS_EXPANSION_BINDING_PARAMETER, .role = "charge_current", .has_value_range = true, .min_value = 100, .max_value = 1000},
    {.key = "charge_voltage", .value_hint = "4000|4100|4200|4350|4400mV", .kind = SOLAR_OS_EXPANSION_BINDING_PARAMETER, .role = "charge_voltage", .has_value_range = true, .min_value = 4000, .max_value = 4400},
};

const solar_os_expansion_driver_t solar_os_axp2101_expansion_driver = {
    .name = "axp2101",
    .category = SOLAR_OS_EXPANSION_CATEGORY_POWER,
    .summary = "AXP2101 PMIC, fuel gauge, and charger",
    .required_capabilities = SOLAR_OS_BOARD_CAP_I2C,
    .probe_supported = true,
    .early = true,
    .binding_specs = binding_specs,
    .binding_spec_count = sizeof(binding_specs) / sizeof(binding_specs[0]),
    .attach = solar_os_axp2101_attach,
    .detach = solar_os_axp2101_detach,
};
