import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import spi
from esphome.const import CONF_ID

DEPENDENCIES = ["spi"]

CONF_REPORT_INTERVAL = "report_interval"
CONF_DWELL_TIME = "dwell_time"

cc2500_scanner_ns = cg.esphome_ns.namespace("cc2500_scanner")
CC2500Scanner = cc2500_scanner_ns.class_(
    "CC2500Scanner", cg.Component, spi.SPIDevice
)

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(CC2500Scanner),
            # How often the peak-hold spectrum is written to the log.
            cv.Optional(
                CONF_REPORT_INTERVAL, default="5s"
            ): cv.positive_time_period_milliseconds,
            # How long RSSI is sampled on each channel before moving on.
            cv.Optional(CONF_DWELL_TIME, default="300us"): cv.All(
                cv.positive_time_period_microseconds,
                cv.Range(
                    min=cv.TimePeriod(microseconds=50),
                    max=cv.TimePeriod(microseconds=5000),
                ),
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
    cg.add(var.set_report_interval(config[CONF_REPORT_INTERVAL]))
    cg.add(var.set_dwell_us(config[CONF_DWELL_TIME]))
