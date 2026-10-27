"""L&S Emotion "Bielefeld" control (8-button remote TxD8 LUS_RC24) via a CC2500.

RF parameters were captured from the remote: 2433.0 MHz, 2-FSK ~10 kBaud,
sync word D391 (sent twice), variable length 14 bytes, CC2500 hardware CRC.
"""
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import spi
from esphome.const import CONF_FREQUENCY, CONF_ID

DEPENDENCIES = ["spi"]
AUTO_LOAD = ["button", "light", "number", "text_sensor"]
MULTI_CONF = False

CONF_LS_BIELEFELD_ID = "ls_bielefeld_id"
CONF_REMOTE_ID = "remote_id"
CONF_DEVIATION = "deviation"
CONF_REPEATS = "repeats"
CONF_DIM_TIME = "dim_time"
CONF_COLOR_TIME = "color_time"

# Bit in payload byte 1, determined from captures (order sun, snowflake, 1..4, on, off).
KEYS = {
    "sonne": 0x01,
    "schneeflocke": 0x02,
    "kanal_1": 0x04,
    "kanal_2": 0x08,
    "kanal_3": 0x10,
    "kanal_4": 0x20,
    "ein": 0x40,
    "aus": 0x80,
}

ls_bielefeld_ns = cg.esphome_ns.namespace("ls_bielefeld")
LSBielefeld = ls_bielefeld_ns.class_("LSBielefeld", cg.Component, spi.SPIDevice)


def _validate_remote_id(value):
    value = cv.string_strict(value)
    cleaned = value.replace(" ", "").replace(":", "").replace("-", "")
    if len(cleaned) != 16:
        raise cv.Invalid("remote_id must be 8 bytes (16 hex digits), e.g. '12 34 56 78 9A BC DE F0'")
    try:
        return list(bytes.fromhex(cleaned))
    except ValueError as err:
        raise cv.Invalid("remote_id must be hexadecimal") from err


CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(LSBielefeld),
            # No default on purpose: the ID belongs to one specific remote.
            # Without it the component only listens and logs the ID of every
            # remote it hears - that is how you find yours.
            cv.Optional(CONF_REMOTE_ID): _validate_remote_id,
            cv.Optional(CONF_FREQUENCY, default="2433.0MHz"): cv.All(
                cv.frequency, cv.Range(min=2400.0e6, max=2483.5e6)
            ),
            # Transmit deviation. Not measurable with the CC2500; 38 kHz is TI's
            # SmartRF default for 10 kBaud, which the rest of the remote's settings match.
            cv.Optional(CONF_DEVIATION, default="38kHz"): cv.All(
                cv.frequency, cv.Range(min=2.0e3, max=200.0e3)
            ),
            # Copies of each frame on air (the remote sends ~4-8).
            cv.Optional(CONF_REPEATS, default=6): cv.int_range(min=1, max=15),
            # Time the control needs to dim from maximum to minimum while a channel
            # key is held (after its ~0.5 s start delay). Used for brightness values.
            cv.Optional(CONF_DIM_TIME, default="1000ms"): cv.All(
                cv.positive_time_period_milliseconds,
                cv.Range(min=cv.TimePeriod(milliseconds=200), max=cv.TimePeriod(seconds=10)),
            ),
            # Same for the colour temperature (sun / snowflake held), coldest to warmest.
            cv.Optional(CONF_COLOR_TIME, default="1200ms"): cv.All(
                cv.positive_time_period_milliseconds,
                cv.Range(min=cv.TimePeriod(milliseconds=200), max=cv.TimePeriod(seconds=10)),
            ),
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
    .extend(spi.spi_device_schema(cs_pin_required=True))
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await spi.register_spi_device(var, config)
    if CONF_REMOTE_ID in config:
        cg.add(var.set_remote_id(*config[CONF_REMOTE_ID]))
    cg.add(var.set_frequency_hz(config[CONF_FREQUENCY]))
    cg.add(var.set_deviation_hz(config[CONF_DEVIATION]))
    cg.add(var.set_repeats(config[CONF_REPEATS]))
    cg.add(var.set_dim_time_ms(config[CONF_DIM_TIME]))
    cg.add(var.set_color_time_ms(config[CONF_COLOR_TIME]))
