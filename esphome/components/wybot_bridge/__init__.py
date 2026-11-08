import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import uart, text_sensor
from esphome.const import CONF_ID

CODEOWNERS = ["@bassrock"]
DEPENDENCIES = ["uart"]
AUTO_LOAD = ["text_sensor"]
MULTI_CONF = False

wybot_ns = cg.esphome_ns.namespace("wybot_bridge")
WybotBridge = wybot_ns.class_("WybotBridge", cg.Component, uart.UARTDevice)

CONF_STATUS = "status_text_sensor"

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(WybotBridge),
            cv.Optional(CONF_STATUS): text_sensor.text_sensor_schema(),
        }
    )
    .extend(uart.UART_DEVICE_SCHEMA)
    .extend(cv.COMPONENT_SCHEMA)
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await uart.register_uart_device(var, config)
    if CONF_STATUS in config:
        ts = await text_sensor.new_text_sensor(config[CONF_STATUS])
        cg.add(var.set_status_sensor(ts))
