import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import uart, sensor, text_sensor, binary_sensor
from esphome.const import CONF_ID, DEVICE_CLASS_RUNNING

CODEOWNERS = ["@bassrock"]
DEPENDENCIES = ["uart"]
AUTO_LOAD = ["sensor", "text_sensor", "binary_sensor"]
MULTI_CONF = False

wybot_ns = cg.esphome_ns.namespace("wybot_bridge")
WybotBridge = wybot_ns.class_("WybotBridge", cg.PollingComponent, uart.UARTDevice)

CONF_HEARTBEAT_INTERVAL = "heartbeat_interval"
CONF_NUMERIC = "numeric_datapoints"
CONF_TEXT = "text_datapoints"
CONF_CLEANING = "cleaning"
CONF_BATTERY_DP = "battery_datapoint"
CONF_ACTIVITY_DP = "activity_datapoint"
CONF_ACTIVITY_THRESHOLD = "activity_threshold"
CONF_CLEANING_TIMEOUT = "cleaning_timeout"
CONF_DP = "dp"
CONF_SCALE = "scale"
CONF_FORMAT = "format"
CONF_OFFSET = "byte_offset"
CONF_WIDTH = "byte_width"
CONF_SIGNED = "signed"

NUMERIC_SCHEMA = sensor.sensor_schema().extend(
    {
        cv.Required(CONF_DP): cv.int_range(min=0, max=255),
        cv.Optional(CONF_SCALE, default=1.0): cv.float_,
        # Slice a specific byte range out of the DP data (LE). Defaults (-1,-1) = whole value.
        cv.Optional(CONF_OFFSET, default=-1): cv.int_range(min=-1, max=16),
        cv.Optional(CONF_WIDTH, default=-1): cv.int_range(min=-1, max=4),
        cv.Optional(CONF_SIGNED, default=False): cv.boolean,
    }
)

TEXT_SCHEMA = text_sensor.text_sensor_schema().extend(
    {
        cv.Required(CONF_DP): cv.int_range(min=0, max=255),
        cv.Optional(CONF_FORMAT, default="hex"): cv.one_of("hex", "int", lower=True),
    }
)

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(WybotBridge),
            cv.Optional(CONF_HEARTBEAT_INTERVAL, default="2s"): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_NUMERIC): cv.ensure_list(NUMERIC_SCHEMA),
            cv.Optional(CONF_TEXT): cv.ensure_list(TEXT_SCHEMA),
            cv.Optional(CONF_CLEANING): binary_sensor.binary_sensor_schema(
                device_class=DEVICE_CLASS_RUNNING
            ),
            cv.Optional(CONF_BATTERY_DP, default=50): cv.int_range(min=0, max=255),
            cv.Optional(CONF_ACTIVITY_DP, default=111): cv.int_range(min=0, max=255),
            cv.Optional(CONF_ACTIVITY_THRESHOLD, default=16): cv.int_range(min=1, max=100000),
            cv.Optional(CONF_CLEANING_TIMEOUT, default="6min"): cv.positive_time_period_milliseconds,
        }
    )
    .extend(uart.UART_DEVICE_SCHEMA)
    .extend(cv.polling_component_schema("500ms"))
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await uart.register_uart_device(var, config)
    cg.add(var.set_heartbeat_interval(config[CONF_HEARTBEAT_INTERVAL]))
    cg.add(var.set_battery_dp(config[CONF_BATTERY_DP]))
    cg.add(var.set_activity_dp(config[CONF_ACTIVITY_DP]))
    cg.add(var.set_activity_threshold(config[CONF_ACTIVITY_THRESHOLD]))
    cg.add(var.set_cleaning_timeout(config[CONF_CLEANING_TIMEOUT]))

    if CONF_CLEANING in config:
        bs = await binary_sensor.new_binary_sensor(config[CONF_CLEANING])
        cg.add(var.set_cleaning_sensor(bs))

    for conf in config.get(CONF_NUMERIC, []):
        s = await sensor.new_sensor(conf)
        cg.add(
            var.add_numeric(
                conf[CONF_DP], s, conf[CONF_SCALE], conf[CONF_OFFSET], conf[CONF_WIDTH], conf[CONF_SIGNED]
            )
        )

    for conf in config.get(CONF_TEXT, []):
        ts = await text_sensor.new_text_sensor(conf)
        cg.add(var.add_text(conf[CONF_DP], ts, conf[CONF_FORMAT] == "hex"))
