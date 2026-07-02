import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import spi, switch, number
from esphome.const import (
    CONF_ID,
    CONF_CS_PIN,
    CONF_FREQUENCY,
)
from esphome import pins

DEPENDENCIES = ["spi"]
AUTO_LOAD = ["switch", "number"]

proflame2_ns = cg.esphome_ns.namespace("proflame2")
ProFlame2Component = proflame2_ns.class_(
    "ProFlame2Component", cg.Component, spi.SPIDevice
)

# Switch types
ProFlame2PowerSwitch = proflame2_ns.class_(
    "ProFlame2PowerSwitch", switch.Switch, cg.Component
)
ProFlame2PilotSwitch = proflame2_ns.class_(
    "ProFlame2PilotSwitch", switch.Switch, cg.Component
)
ProFlame2AuxSwitch = proflame2_ns.class_(
    "ProFlame2AuxSwitch", switch.Switch, cg.Component
)
ProFlame2FrontSwitch = proflame2_ns.class_(
    "ProFlame2FrontSwitch", switch.Switch, cg.Component
)
ProFlame2ThermostatSwitch = proflame2_ns.class_(
    "ProFlame2ThermostatSwitch", switch.Switch, cg.Component
)

# Number types
ProFlame2FlameNumber = proflame2_ns.class_(
    "ProFlame2FlameNumber", number.Number, cg.Component
)
ProFlame2FanNumber = proflame2_ns.class_(
    "ProFlame2FanNumber", number.Number, cg.Component
)
ProFlame2LightNumber = proflame2_ns.class_(
    "ProFlame2LightNumber", number.Number, cg.Component
)

CONF_GDO0_PIN = "gdo0_pin"
CONF_SERIAL_NUMBER = "serial_number"
CONF_POWER = "power"
CONF_PILOT = "pilot"
CONF_AUX = "aux"
CONF_FRONT = "front"
CONF_THERMOSTAT = "thermostat"
CONF_FLAME = "flame"
CONF_FAN = "fan"
CONF_LIGHT = "light"
# Error-detection word constants (4-bit each). Device specific: derive them from an
# rtl_433 capture of the paired remote (see protocol/README.md). Defaults are the
# smartfire reference device's values.
CONF_CHECKSUM_C1 = "checksum_c1"
CONF_CHECKSUM_D1 = "checksum_d1"
CONF_CHECKSUM_C2 = "checksum_c2"
CONF_CHECKSUM_D2 = "checksum_d2"

nibble = cv.All(cv.hex_int, cv.Range(min=0, max=15))

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(ProFlame2Component),
        cv.Required(CONF_CS_PIN): pins.gpio_output_pin_schema,
        cv.Optional(CONF_GDO0_PIN): pins.gpio_input_pin_schema,
        cv.Optional(CONF_SERIAL_NUMBER, default=0x12345678): cv.hex_uint32_t,
        cv.Optional(CONF_FREQUENCY, default="314.973MHz"): cv.All(
            cv.frequency, cv.Range(min=300e6, max=348e6)
        ),
        cv.Optional(CONF_CHECKSUM_C1, default=0x0D): nibble,
        cv.Optional(CONF_CHECKSUM_D1, default=0x00): nibble,
        cv.Optional(CONF_CHECKSUM_C2, default=0x00): nibble,
        cv.Optional(CONF_CHECKSUM_D2, default=0x07): nibble,
        cv.Optional(CONF_POWER): switch.switch_schema(ProFlame2PowerSwitch),
        cv.Optional(CONF_PILOT): switch.switch_schema(ProFlame2PilotSwitch),
        cv.Optional(CONF_AUX): switch.switch_schema(ProFlame2AuxSwitch),
        cv.Optional(CONF_FRONT): switch.switch_schema(ProFlame2FrontSwitch),
        cv.Optional(CONF_THERMOSTAT): switch.switch_schema(ProFlame2ThermostatSwitch),
        cv.Optional(CONF_FLAME): number.number_schema(ProFlame2FlameNumber),
        cv.Optional(CONF_FAN): number.number_schema(ProFlame2FanNumber),
        cv.Optional(CONF_LIGHT): number.number_schema(ProFlame2LightNumber),
    }
).extend(spi.spi_device_schema())


SWITCH_TYPES = {
    CONF_POWER: "set_power_switch",
    CONF_PILOT: "set_pilot_switch",
    CONF_AUX: "set_aux_switch",
    CONF_FRONT: "set_front_switch",
    CONF_THERMOSTAT: "set_thermostat_switch",
}

NUMBER_TYPES = {
    CONF_FLAME: "set_flame_number",
    CONF_FAN: "set_fan_number",
    CONF_LIGHT: "set_light_number",
}


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await spi.register_spi_device(var, config)

    cg.add(var.set_serial_number(config[CONF_SERIAL_NUMBER]))
    cg.add(var.set_frequency(config[CONF_FREQUENCY] / 1e6))
    cg.add(
        var.set_checksum_constants(
            config[CONF_CHECKSUM_C1],
            config[CONF_CHECKSUM_D1],
            config[CONF_CHECKSUM_C2],
            config[CONF_CHECKSUM_D2],
        )
    )

    if CONF_GDO0_PIN in config:
        pin = await cg.gpio_pin_expression(config[CONF_GDO0_PIN])
        cg.add(var.set_gdo0_pin(pin))

    for key, setter in SWITCH_TYPES.items():
        if key in config:
            conf = config[key]
            sw = cg.new_Pvariable(conf[CONF_ID])
            await cg.register_component(sw, conf)
            await switch.register_switch(sw, conf)
            cg.add(sw.set_parent(var))
            cg.add(getattr(var, setter)(sw))

    for key, setter in NUMBER_TYPES.items():
        if key in config:
            conf = config[key]
            num = cg.new_Pvariable(conf[CONF_ID])
            await cg.register_component(num, conf)
            await number.register_number(
                num,
                conf,
                min_value=0,
                max_value=6,
                step=1,
            )
            cg.add(num.set_parent(var))
            cg.add(getattr(var, setter)(num))
