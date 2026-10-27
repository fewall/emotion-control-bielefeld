import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import text_sensor

from . import CONF_LS_BIELEFELD_ID, LSBielefeld

DEPENDENCIES = ["ls_bielefeld"]

# Shows the key currently pressed on the physical remote ("Ein", "Kanal 1",
# "Sonne (gehalten)", ...) and goes back to "" when it is released, so every
# press is a visible change in ioBroker / Home Assistant.
CONFIG_SCHEMA = text_sensor.text_sensor_schema(icon="mdi:remote").extend(
    {cv.GenerateID(CONF_LS_BIELEFELD_ID): cv.use_id(LSBielefeld)}
)


async def to_code(config):
    var = await text_sensor.new_text_sensor(config)
    parent = await cg.get_variable(config[CONF_LS_BIELEFELD_ID])
    cg.add(parent.set_key_sensor(var))
