import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID

wybot_pinscan_ns = cg.esphome_ns.namespace("wybot_pinscan")
WybotPinScan = wybot_pinscan_ns.class_("WybotPinScan", cg.Component)

CONF_UART_NUM = "uart_num"
CONF_CANDIDATE_PINS = "candidate_pins"
CONF_BAUDS = "bauds"
CONF_DWELL_MS = "dwell_ms"

# Explicit allow-list so a typo can never name a flash (11-17) or USB-JTAG (18/19) pin.
# Permitted: general GPIOs 0-10 (incl. strapping 2/8/9 -- passive input-enable at runtime is
# harmless) and 20/21 (free: the logger/console is on USB-Serial-JTAG, not UART0).
_SAFE_PINS = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 20, 21]


def _safe_pin(value):
    v = cv.int_(value)
    if v not in _SAFE_PINS:
        raise cv.Invalid(
            f"GPIO{v} is not probe-safe (flash 11-17 / USB-JTAG 18-19 are excluded); "
            f"allowed: {_SAFE_PINS}"
        )
    return v


CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(WybotPinScan),
        # UART1 by default: UART0/USB-CDC is the console on the C3.
        cv.Optional(CONF_UART_NUM, default=1): cv.int_range(min=0, max=1),
        # First pass: GPIOs most plausibly wired to the GD32. Add 20/21 (2nd pass) and 2/8/9
        # (3rd pass) via the yaml if the first pass finds nothing.
        cv.Optional(CONF_CANDIDATE_PINS, default=[0, 1, 3, 4, 5, 6, 7, 10]): cv.ensure_list(
            _safe_pin
        ),
        # 9600 is the Tuya default; MCUs may only answer when polled, so dwell long.
        cv.Optional(CONF_BAUDS, default=[115200, 9600]): cv.ensure_list(cv.positive_int),
        cv.Optional(CONF_DWELL_MS, default=20000): cv.positive_int,
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_uart_num(config[CONF_UART_NUM]))
    cg.add(var.set_pins(config[CONF_CANDIDATE_PINS]))
    cg.add(var.set_bauds(config[CONF_BAUDS]))
    cg.add(var.set_dwell_ms(config[CONF_DWELL_MS]))
