#pragma once
// WyBot DS20 dock <-> GD32 motor MCU UART bridge.
//
// Re-emits Tuya-style datapoint (DP) frames (documented in ../../docs/protocol.md) on the
// C3<->MCU UART, and parses DP reports coming back from the MCU into Home Assistant.
//
// Frame (from pywybot BLE/MQTT framing; the UART framing is assumed identical and is to be
// CONFIRMED against real captured MCU traffic once this runs on the dock):
//   AA 55 | cmd(2, big-endian) | len(2, little-endian) | payload | checksum(1)
//   payload = dp_id(1) dp_type(1) dp_len(2, big-endian) data[dp_len]
//   checksum = sum(all bytes from first AA through last payload byte) & 0xFF   (TO CONFIRM)

#include "esphome/core/component.h"
#include "esphome/core/log.h"
#include "esphome/components/uart/uart.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include <vector>

namespace esphome {
namespace wybot_bridge {

class WybotBridge : public Component, public uart::UARTDevice {
 public:
  void set_status_sensor(text_sensor::TextSensor *s) { this->status_ = s; }

  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::LATE; }

  // Low-level DP op. cmd mirrors the MQTT/BLE mapping (4=write, 9=query, 5=report).
  void send_dp(uint16_t cmd, uint8_t dp_id, uint8_t dp_type, const std::vector<uint8_t> &data);

  // Known DPs (from pywybot). Values/modes are placeholders to confirm from capture.
  void start_clean() { this->send_dp(CMD_WRITE, 0, DP_BOOL, {1}); }
  void stop_clean() { this->send_dp(CMD_WRITE, 0, DP_BOOL, {0}); }
  void set_mode(uint8_t m) { this->send_dp(CMD_WRITE, 1, DP_ENUM, {m}); }
  void query(uint8_t dp_id) { this->send_dp(CMD_QUERY, dp_id, DP_RAW, {}); }

 protected:
  static const uint16_t CMD_WRITE = 0x0004;
  static const uint16_t CMD_QUERY = 0x0009;
  static const uint16_t CMD_REPORT = 0x0005;
  static const uint8_t DP_RAW = 0, DP_BOOL = 1, DP_INT = 2, DP_ENUM = 4;
  // enum (prvalues) so these can be ODR-used (push_back takes const&) without out-of-line defs
  enum : uint8_t { HDR0 = 0xAA, HDR1 = 0x55 };

  static uint8_t checksum(const std::vector<uint8_t> &f, size_t upto);
  void handle_frame(const std::vector<uint8_t> &f);

  std::vector<uint8_t> rx_;
  text_sensor::TextSensor *status_{nullptr};
};

}  // namespace wybot_bridge
}  // namespace esphome
