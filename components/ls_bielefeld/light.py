import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import light
from esphome.const import (
    CONF_CHANNEL,
    CONF_DEFAULT_TRANSITION_LENGTH,
    CONF_GAMMA_CORRECT,
    CONF_OUTPUT_ID,
)

from . import CONF_LS_BIELEFELD_ID, LSBielefeld, ls_bielefeld_ns

DEPENDENCIES = ["ls_bielefeld"]

LSChannelLight = ls_bielefeld_ns.class_("LSChannelLight", light.LightOutput)

# One lamp channel of the Bielefeld control as a dimmable light. The control
# gives no feedback, so on/off and brightness are estimates: they follow
# everything the ESP sends and everything it hears from the real remote
# (channel key = toggle, holding it = dimming, "Ein"/"Aus" = all channels).
# A new brightness is reached by holding the channel key for a calculated time.
CONFIG_SCHEMA = light.BRIGHTNESS_ONLY_LIGHT_SCHEMA.extend(
    {
        # Transitions and gamma make no sense here: the control dims by itself.
        cv.Optional(CONF_DEFAULT_TRANSITION_LENGTH, default="0s"): cv.positive_time_period_milliseconds,
        cv.Optional(CONF_GAMMA_CORRECT, default=1.0): cv.positive_float,
        cv.GenerateID(CONF_OUTPUT_ID): cv.declare_id(LSChannelLight),
        cv.GenerateID(CONF_LS_BIELEFELD_ID): cv.use_id(LSBielefeld),
        cv.Required(CONF_CHANNEL): cv.int_range(min=1, max=4),
    }
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_OUTPUT_ID])
    await light.register_light(var, config)
    parent = await cg.get_variable(config[CONF_LS_BIELEFELD_ID])
    cg.add(var.set_parent(parent))
    cg.add(var.set_channel(config[CONF_CHANNEL]))
    cg.add(parent.register_light(config[CONF_CHANNEL], var))
