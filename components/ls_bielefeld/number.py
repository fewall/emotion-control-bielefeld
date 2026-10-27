import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import number
from esphome.const import UNIT_PERCENT

from . import CONF_LS_BIELEFELD_ID, LSBielefeld, ls_bielefeld_ns

DEPENDENCIES = ["ls_bielefeld"]

LSColorNumber = ls_bielefeld_ns.class_("LSColorNumber", number.Number)

# Colour temperature of all lamps, 0 % = coldest, 100 % = warmest. Like the
# brightness this is an estimate, reached by holding sun or snowflake.
CONFIG_SCHEMA = number.number_schema(
    LSColorNumber, icon="mdi:thermometer", unit_of_measurement=UNIT_PERCENT
).extend(
    {
        cv.GenerateID(CONF_LS_BIELEFELD_ID): cv.use_id(LSBielefeld),
    }
)


async def to_code(config):
    var = await number.new_number(config, min_value=0, max_value=100, step=1)
    parent = await cg.get_variable(config[CONF_LS_BIELEFELD_ID])
    cg.add(var.set_parent(parent))
    cg.add(parent.set_color_number(var))
