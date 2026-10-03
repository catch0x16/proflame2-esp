import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor, button, remote_base, sensor, switch
from esphome.const import CONF_ID, STATE_CLASS_MEASUREMENT

# The radio is driven by ESPHome's cc1101 component; this component only builds the
# ProFlame 2 burst and sends it through a remote_transmitter wired to the CC1101's GDO0.
# The optional `receive:` block also listens on a remote_receiver for a second ProFlame 2
# remote and re-sends its commands under our own serial number (remote proxy).
# The remote owns the fireplace state: Home Assistant sees it through read-only
# sensors and can only force the power off (the override switch / force-off button).
DEPENDENCIES = ["remote_transmitter"]
AUTO_LOAD = ["binary_sensor", "button", "sensor", "switch"]

proflame2_ns = cg.esphome_ns.namespace("proflame2")
ProFlame2Component = proflame2_ns.class_(
    "ProFlame2Component",
    cg.Component,
    remote_base.RemoteTransmittable,
    remote_base.RemoteReceiverListener,
)

ProFlame2OverrideSwitch = proflame2_ns.class_(
    "ProFlame2OverrideSwitch", switch.Switch, cg.Parented.template(ProFlame2Component)
)
ProFlame2ForceOffButton = proflame2_ns.class_(
    "ProFlame2ForceOffButton", button.Button, cg.Parented.template(ProFlame2Component)
)

CONF_SERIAL_NUMBER = "serial_number"
CONF_POWER = "power"
CONF_PILOT = "pilot"
CONF_AUX = "aux"
CONF_FRONT = "front"
CONF_THERMOSTAT = "thermostat"
CONF_FLAME = "flame"
CONF_FAN = "fan"
CONF_LIGHT = "light"
CONF_OVERRIDE = "override"
CONF_FORCE_OFF = "force_off"
# Error-detection word constants (4-bit each). Device specific: derive them from an
# rtl_433 capture of the paired remote (see README "Checksum Constants"). Defaults are the
# smartfire reference device's values.
CONF_CHECKSUM_C1 = "checksum_c1"
CONF_CHECKSUM_D1 = "checksum_d1"
CONF_CHECKSUM_C2 = "checksum_c2"
CONF_CHECKSUM_D2 = "checksum_d2"
CONF_RECEIVE = "receive"

nibble = cv.All(cv.hex_int, cv.Range(min=0, max=15))

# Flame / fan / light level, 0-6.
LEVEL_SCHEMA = sensor.sensor_schema(
    accuracy_decimals=0, state_class=STATE_CLASS_MEASUREMENT
)

# The external remote whose frames we accept. Its serial and checksum constants are
# derived exactly like our own (see README "Checksum Constants"), from a capture of
# that remote. No defaults: a wrong value silently drops every frame.
RECEIVE_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_SERIAL_NUMBER): cv.hex_uint32_t,
        cv.Required(CONF_CHECKSUM_C1): nibble,
        cv.Required(CONF_CHECKSUM_D1): nibble,
        cv.Required(CONF_CHECKSUM_C2): nibble,
        cv.Required(CONF_CHECKSUM_D2): nibble,
    }
).extend(remote_base.REMOTE_LISTENER_SCHEMA)


def _validate_receive(config):
    if CONF_RECEIVE in config:
        rx_serial = config[CONF_RECEIVE][CONF_SERIAL_NUMBER] & 0xFFFFFF
        if rx_serial == config[CONF_SERIAL_NUMBER] & 0xFFFFFF:
            # We would hear our own retransmission and re-send it forever.
            raise cv.Invalid(
                "receive serial_number must differ from serial_number",
                path=[CONF_RECEIVE, CONF_SERIAL_NUMBER],
            )
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(ProFlame2Component),
            cv.Optional(CONF_SERIAL_NUMBER, default=0x12345678): cv.hex_uint32_t,
            cv.Optional(CONF_CHECKSUM_C1, default=0x0D): nibble,
            cv.Optional(CONF_CHECKSUM_D1, default=0x00): nibble,
            cv.Optional(CONF_CHECKSUM_C2, default=0x00): nibble,
            cv.Optional(CONF_CHECKSUM_D2, default=0x07): nibble,
            cv.Optional(CONF_POWER): binary_sensor.binary_sensor_schema(),
            cv.Optional(CONF_PILOT): binary_sensor.binary_sensor_schema(),
            cv.Optional(CONF_AUX): binary_sensor.binary_sensor_schema(),
            cv.Optional(CONF_FRONT): binary_sensor.binary_sensor_schema(),
            cv.Optional(CONF_THERMOSTAT): binary_sensor.binary_sensor_schema(),
            cv.Optional(CONF_FLAME): LEVEL_SCHEMA,
            cv.Optional(CONF_FAN): LEVEL_SCHEMA,
            cv.Optional(CONF_LIGHT): LEVEL_SCHEMA,
            cv.Optional(CONF_OVERRIDE): switch.switch_schema(ProFlame2OverrideSwitch),
            cv.Optional(CONF_FORCE_OFF): button.button_schema(ProFlame2ForceOffButton),
            cv.Optional(CONF_RECEIVE): RECEIVE_SCHEMA,
        }
    )
    .extend(remote_base.REMOTE_TRANSMITTABLE_SCHEMA)
    .extend(cv.COMPONENT_SCHEMA),
    _validate_receive,
)


BINARY_SENSOR_TYPES = {
    CONF_POWER: "set_power_sensor",
    CONF_PILOT: "set_pilot_sensor",
    CONF_AUX: "set_aux_sensor",
    CONF_FRONT: "set_front_sensor",
    CONF_THERMOSTAT: "set_thermostat_sensor",
}

SENSOR_TYPES = {
    CONF_FLAME: "set_flame_sensor",
    CONF_FAN: "set_fan_sensor",
    CONF_LIGHT: "set_light_sensor",
}


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await remote_base.register_transmittable(var, config)

    cg.add(var.set_serial_number(config[CONF_SERIAL_NUMBER]))
    cg.add(
        var.set_checksum_constants(
            config[CONF_CHECKSUM_C1],
            config[CONF_CHECKSUM_D1],
            config[CONF_CHECKSUM_C2],
            config[CONF_CHECKSUM_D2],
        )
    )

    if CONF_RECEIVE in config:
        rx = config[CONF_RECEIVE]
        await remote_base.register_listener(var, rx)
        cg.add(
            var.set_receive_config(
                rx[CONF_SERIAL_NUMBER],
                rx[CONF_CHECKSUM_C1],
                rx[CONF_CHECKSUM_D1],
                rx[CONF_CHECKSUM_C2],
                rx[CONF_CHECKSUM_D2],
            )
        )

    for key, setter in BINARY_SENSOR_TYPES.items():
        if key in config:
            bs = await binary_sensor.new_binary_sensor(config[key])
            cg.add(getattr(var, setter)(bs))

    for key, setter in SENSOR_TYPES.items():
        if key in config:
            sens = await sensor.new_sensor(config[key])
            cg.add(getattr(var, setter)(sens))

    if CONF_OVERRIDE in config:
        sw = await switch.new_switch(config[CONF_OVERRIDE])
        await cg.register_parented(sw, var)
        cg.add(var.set_override_switch(sw))

    if CONF_FORCE_OFF in config:
        btn = await button.new_button(config[CONF_FORCE_OFF])
        await cg.register_parented(btn, var)
