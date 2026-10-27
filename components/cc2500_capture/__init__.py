import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import spi
from esphome.const import CONF_FREQUENCY, CONF_ID

DEPENDENCIES = ["spi"]

CONF_PROFILE = "profile"
CONF_PROFILE_TIME = "profile_time"
NUM_PROFILES = 10

cc2500_capture_ns = cg.esphome_ns.namespace("cc2500_capture")
CC2500Capture = cc2500_capture_ns.class_(
    "CC2500Capture", cg.Component, spi.SPIDevice
)

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(CC2500Capture),
            cv.Optional(CONF_FREQUENCY, default="2433.0MHz"): cv.All(
                cv.frequency, cv.Range(min=2400.0e6, max=2483.5e6)
            ),
            # -1 = cycle through all profiles automatically, 0..9 = fixed profile
            cv.Optional(CONF_PROFILE, default=-1): cv.int_range(
                min=-1, max=NUM_PROFILES - 1
            ),
            cv.Optional(
                CONF_PROFILE_TIME, default="20s"
            ): cv.positive_time_period_milliseconds,
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
    .extend(spi.spi_device_schema(cs_pin_required=True))
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await spi.register_spi_device(var, config)
    cg.add(var.set_frequency_hz(config[CONF_FREQUENCY]))
    cg.add(var.set_fixed_profile(config[CONF_PROFILE]))
    cg.add(var.set_profile_time(config[CONF_PROFILE_TIME]))
