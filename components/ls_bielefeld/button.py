import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import button
from esphome.const import CONF_KEY

from . import CONF_LS_BIELEFELD_ID, KEYS, LSBielefeld, ls_bielefeld_ns

DEPENDENCIES = ["ls_bielefeld"]

CONF_HOLD = "hold"

LSButton = ls_bielefeld_ns.class_("LSButton", button.Button)

CONFIG_SCHEMA = button.button_schema(LSButton, icon="mdi:remote").extend(
    {
        cv.GenerateID(CONF_LS_BIELEFELD_ID): cv.use_id(LSBielefeld),
        # One key or a list of keys pressed together, e.g. [kanal_1, kanal_2]
        cv.Required(CONF_KEY): cv.All(
            cv.ensure_list(cv.enum(KEYS, lower=True)), cv.Length(min=1)
        ),
        # 0 = short press; otherwise the key is held this long (e.g. for dimming).
        cv.Optional(CONF_HOLD, default="0ms"): cv.All(
            cv.positive_time_period_milliseconds,
            cv.Range(max=cv.TimePeriod(seconds=10)),
        ),
    }
)


async def to_code(config):
    var = await button.new_button(config)
    parent = await cg.get_variable(config[CONF_LS_BIELEFELD_ID])
    cg.add(var.set_parent(parent))
    mask = 0
    for key in config[CONF_KEY]:
        mask |= KEYS[key]
    cg.add(var.set_key(mask))
    cg.add(var.set_hold_ms(config[CONF_HOLD]))
