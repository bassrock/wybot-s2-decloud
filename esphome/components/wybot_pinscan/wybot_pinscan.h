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
#include "esphome/core/defines.h"
#include "esphome/core/log.h"
#ifdef USE_WIFI
#include "esphome/components/wifi/wifi_component.h"
#endif
#ifdef USE_API
#include "esphome/components/api/api_server.h"
#endif
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
  // Probing is armed by an explicit switch (default OFF). Until armed AND a HA/API client is
  // connected, no GPIO is touched -- on a sealed dock the device must first prove it is
  // reachable and stay watched before it ever re-muxes a pin.
  void set_enabled(bool e) { this->enabled_ = e; }
  bool is_enabled() const { return this->enabled_; }

  // Logs the partition layout + running/next-OTA slot at boot: confirms the dock is dual-OTA
  // (else stage-2 ESPHome OTA could fail with NO_UPDATE_PARTITION) before we rely on it.
  void log_partitions();
  // Zero-upload escape hatch: boot the *other* app slot. Right after the stage-1 takeover the
  // stock image is still in that slot, so this reverts to stock without any upload.
  void boot_other_slot();

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
  bool enabled_{false};
  bool was_gated_{true};  // so we log once when probing actually starts/stops
};

}  // namespace wybot_pinscan
}  // namespace esphome
