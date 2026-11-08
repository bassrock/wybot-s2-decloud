import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID

wybot_pinscan_ns = cg.esphome_ns.namespace("wybot_pinscan")
WybotPinScan = wybot_pinscan_ns.class_("WybotPinScan", cg.Component)

CONF_UART_NUM = "uart_num"
CONF_CANDIDATE_PINS = "candidate_pins"
CONF_BAUDS = "bauds"
CONF_DWELL_MS = "dwell_ms"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(WybotPinScan),
        # UART1 by default: UART0/USB-CDC is the console on the C3.
        cv.Optional(CONF_UART_NUM, default=1): cv.int_range(min=0, max=1),
        # C3-MINI-1 GPIOs that are plausibly wired to the GD32: excludes flash (11-17),
        # USB (18/19), console UART0 (20/21) and strapping (2/8/9). Override if needed.
        cv.Optional(CONF_CANDIDATE_PINS, default=[0, 1, 3, 4, 5, 6, 7, 10]): cv.ensure_list(
            cv.int_range(min=0, max=21)
        ),
        cv.Optional(CONF_BAUDS, default=[115200]): cv.ensure_list(cv.positive_int),
        cv.Optional(CONF_DWELL_MS, default=4000): cv.positive_int,
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_uart_num(config[CONF_UART_NUM]))
    cg.add(var.set_pins(config[CONF_CANDIDATE_PINS]))
    cg.add(var.set_bauds(config[CONF_BAUDS]))
    cg.add(var.set_dwell_ms(config[CONF_DWELL_MS]))
