#pragma once
// WyBot dock C3<->GD32 UART pin finder.
//
// The dock's ESP32-C3 bridges WiFi/BLE to the GD32 motor MCU over a UART whose GPIOs are
// unknown (the stock firmware drives UART1 directly; see docs/protocol.md). This component
// walks a set of candidate RX GPIOs x bauds on a hardware UART, listening passively (TX is
// never driven), and reports which pin/baud carries Tuya-style AA55 DP frames. That pin is
// the C3's RX (= the MCU's TX); the paired TX is then confirmed in a second phase by sending
// a DP query and watching for a reply. It also hex-dumps what it hears, so the real framing,
// baud and DP ids are captured at the same time.
//
// Passive and safe: only one GPIO at a time is routed to UART RX as an input; nothing is
// driven, so there is no bus contention with the MCU.

#include "esphome/core/component.h"
#include "esphome/core/log.h"
#include <driver/uart.h>
#include <vector>

namespace esphome {
namespace wybot_pinscan {

class WybotPinScan : public Component {
 public:
  void set_uart_num(int n) { this->uart_num_ = (uart_port_t) n; }
  void set_pins(std::vector<int> p) { this->pins_ = std::move(p); }
  void set_bauds(std::vector<int> b) { this->bauds_ = std::move(b); }
  void set_dwell_ms(uint32_t d) { this->dwell_ = d; }

  float get_setup_priority() const override { return setup_priority::LATE; }
  void setup() override;
  void loop() override;
  void dump_config() override;

 protected:
  void scan_window_();
  void advance_();

  uart_port_t uart_num_{UART_NUM_1};
  std::vector<int> pins_;
  std::vector<int> bauds_;
  uint32_t dwell_{4000};
  uint32_t last_switch_{0};
  int pi_{0};
  int bi_{-1};  // advance_() runs first and bumps this to 0
  uint32_t bytes_seen_{0};
  uint32_t aa55_{0};
  bool installed_{false};
};

}  // namespace wybot_pinscan
}  // namespace esphome
