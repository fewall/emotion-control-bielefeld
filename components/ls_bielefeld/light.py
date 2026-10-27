import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import light
from esphome.const import CONF_CHANNEL, CONF_OUTPUT_ID

from . import CONF_LS_BIELEFELD_ID, LSBielefeld, ls_bielefeld_ns

DEPENDENCIES = ["ls_bielefeld"]

LSChannelLight = ls_bielefeld_ns.class_("LSChannelLight", light.LightOutput)

# One lamp channel of the Bielefeld control as an on/off light. The control
# gives no feedback, so the state is an estimate: it follows everything the
# ESP sends and everything it hears from the real remote (channel key =
# toggle, holding it = dimming, "Ein"/"Aus" = all channels).
CONFIG_SCHEMA = light.BINARY_LIGHT_SCHEMA.extend(
    {
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
