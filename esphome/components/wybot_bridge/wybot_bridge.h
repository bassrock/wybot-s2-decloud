#pragma once
// WyBot DS20 dock <-> GD32 MCU UART bridge (local, cloud-free).
//
// PROTOCOL (recovered by static RE of the stock C3 image + confirmed live on the dock):
//   Link: UART0 @ 115200 8N1, C3 TX=GPIO21 / C3 RX=GPIO20. The MCU is a SLAVE: it idles its TX
//   high and only answers when polled, so the bridge must drive the conversation.
//   Frame:  AA 55 00 | cmd(1) | plen(2, little-endian) | payload[plen] | cksum(1)
//           cksum = sum(every byte from the first AA through the last payload byte) & 0xFF
//   Commands (C3 -> MCU):  0x00 heartbeat,  0x09 query DP (payload = dp_id),  0x04 write DP.
//   Reports (MCU -> C3):   0x00 heartbeat reply (const 10 10 00),  0x05 DP report.
//   DP report payload = one or more datapoints concatenated:
//           dp_id(1) | dp_type(1) | dp_len(1) | data[dp_len]
//           types: 0x01 bool(1B), 0x02 int(dp_len B, LITTLE-endian, signed), 0x04 enum(1B),
//                  0x05 bitmap(4B), 0x00 raw.
//
// The bridge round-robin polls a configured DP set, parses the reports into HA entities, and
// sends periodic heartbeats as a keep-alive. Writes (control) are exposed as services.

#include "esphome/core/component.h"
#include "esphome/core/log.h"
#include "esphome/components/uart/uart.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include <map>
#include <vector>

namespace esphome {
namespace wybot_bridge {

class WybotBridge : public PollingComponent, public uart::UARTDevice {
 public:
  void set_heartbeat_interval(uint32_t ms) { this->hb_interval_ = ms; }

  // --- Derived "robot is actively cleaning" detection ---------------------------------------
  // The dock reports the robot's DP0 status over a short-range link that goes STALE the moment the
  // robot swims away, so DP0 reads "stopped" mid-clean. But the dock reports the robot BATTERY
  // (battery_dp byte1) and a fast run-counter (activity_dp) accurately. The motors only draw the
  // battery down, and the run-counter only climbs fast, while the robot is actually working. So we
  // latch "cleaning" on a battery drop OR a fast run-counter advance (never while charging), and
  // auto-clear it after cleaning_timeout with no further activity.
  void set_cleaning_sensor(binary_sensor::BinarySensor *s) { this->cleaning_ = s; }
  void set_battery_dp(uint8_t d) { this->battery_dp_ = d; }
  void set_activity_dp(uint8_t d) { this->activity_dp_ = d; }
  void set_activity_threshold(int t) { this->activity_threshold_ = t; }
  void set_cleaning_timeout(uint32_t ms) { this->cleaning_timeout_ms_ = ms; }

  // --- Derived "robot is charging" detection -------------------------------------------------
  // On a STANDARD dock the robot sets battery_dp byte0 (charge_state) to 1 while charging. On the
  // SOLAR dock it leaves charge_state at 0 the whole time, so that byte alone reads "not charging"
  // even as the pack fills. We therefore treat the robot as charging when charge_state==1 OR the
  // battery % ticks UP while docked, and latch it off on a battery drop, a "charged" state, 100%,
  // or charge_timeout with no further rise. This mirrors the cleaning detector (battery down) in
  // reverse (battery up).
  void set_charging_sensor(binary_sensor::BinarySensor *s) { this->charging_sensor_ = s; }
  void set_charge_timeout(uint32_t ms) { this->charge_timeout_ms_ = ms; }

  // dp -> entity registrations (from __init__.py). A DP may feed several numeric sensors,
  // each extracting a different slice (offset/width) of the DP data (e.g. DP50 byte1 =
  // charge state, byte2 = battery %). offset<0 + width<0 means "whole value".
  void add_numeric(uint8_t dp, sensor::Sensor *s, float scale, int offset, int width, bool is_signed,
                   uint8_t tier) {
    this->num_.push_back({dp, s, scale, offset, width, is_signed});
    this->register_poll_(dp, tier);
  }
  void add_text(uint8_t dp, text_sensor::TextSensor *s, bool as_hex, uint8_t tier) {
    this->txt_[dp] = {s, as_hex};
    this->register_poll_(dp, tier);
  }
  void set_static_interval(uint32_t ms) { this->static_interval_ms_ = ms; }
  // Latest raw DP data (hex), for template/select/button lambdas in YAML.
  bool dp_byte(uint8_t dp, int offset, uint8_t &out) const {
    auto it = this->last_data_.find(dp);
    if (it == this->last_data_.end() || offset < 0 || (size_t) offset >= it->second.size())
      return false;
    out = it->second[offset];
    return true;
  }
  int dp_int(uint8_t dp) const {  // whole-value little-endian signed, or -1 if absent
    auto it = this->last_data_.find(dp);
    if (it == this->last_data_.end() || it->second.empty())
      return -1;
    int32_t v = 0;
    const auto &d = it->second;
    for (size_t k = 0; k < d.size() && k < 4; k++)
      v |= ((int32_t) d[k]) << (8 * k);
    return v;
  }

  void setup() override;
  void loop() override;
  void update() override;  // PollingComponent: fires one query per poll_interval
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::LATE; }

  // Build + send a framed command.
  void send_cmd(uint8_t cmd, const std::vector<uint8_t> &payload);
  void query_dp(uint8_t dp) { this->send_cmd(0x09, {dp}); }
  void heartbeat() { this->send_cmd(0x00, {}); }

  // DP writes (control). value encoded per type.
  void write_bool(uint8_t dp, bool v) {
    this->send_cmd(0x04, dp_frame_(dp, 0x01, {(uint8_t) (v ? 1 : 0)}));
    this->bump_fast_();
  }
  void write_enum(uint8_t dp, uint8_t v) {
    this->send_cmd(0x04, dp_frame_(dp, 0x04, {v}));
    this->bump_fast_();
  }
  void write_int(uint8_t dp, int32_t v) {
    this->send_cmd(0x04, dp_frame_(dp, 0x02,
                                   {(uint8_t) (v & 0xFF), (uint8_t) ((v >> 8) & 0xFF),
                                    (uint8_t) ((v >> 16) & 0xFF), (uint8_t) ((v >> 24) & 0xFF)}));
    this->bump_fast_();
  }
  // Escape hatch for exploration from HA: send arbitrary raw bytes verbatim.
  void send_raw(const std::vector<uint8_t> &bytes) { this->write_array(bytes); }

 protected:
  struct NumReg { uint8_t dp; sensor::Sensor *s; float scale; int offset; int width; bool is_signed; };
  struct TxtReg { text_sensor::TextSensor *s; bool hex; };

  static uint8_t checksum_(const std::vector<uint8_t> &f, size_t upto);
  static std::vector<uint8_t> dp_frame_(uint8_t dp, uint8_t type, const std::vector<uint8_t> &data);
  void register_poll_(uint8_t dp, uint8_t tier);  // tier: 0 fast, 1 slow, 2 static
  void bump_fast_();  // after a control write, re-query the status/mode/activity DPs promptly
  void handle_frame_(const std::vector<uint8_t> &f);
  void handle_report_(const std::vector<uint8_t> &payload);
  void update_cleaning_(uint8_t dp, const std::vector<uint8_t> &data);  // battery/run-counter latch
  void set_cleaning_(bool v);
  void set_charging_(bool v);

  std::vector<NumReg> num_;
  std::map<uint8_t, TxtReg> txt_;
  std::map<uint8_t, std::vector<uint8_t>> last_data_;

  // Poll tiers (see __init__.py). tier_of_ keeps the strongest tier requested per DP; the three
  // rings are rebuilt from it in setup(). The slot scheduler spends 4/5 of its slots on fast_ and
  // the 5th on a due static_ DP (else the next slow_ DP). Values reported for a DP with no listener
  // are WARN'd once (warned_) so nothing the device sends is silently dropped.
  std::map<uint8_t, uint8_t> tier_of_;
  std::vector<uint8_t> fast_, slow_, static_;
  std::map<uint8_t, uint32_t> static_last_;  // dp -> last poll millis (static re-poll timer)
  std::vector<uint8_t> priority_;            // DPs to query ASAP (filled by bump_fast_)
  std::map<uint8_t, bool> warned_;           // unlistened DPs already logged
  size_t fast_idx_{0}, slow_idx_{0};
  uint32_t poll_counter_{0};
  uint32_t static_interval_ms_{600000};  // re-poll each static DP every 10 min

  std::vector<uint8_t> rx_;
  uint32_t hb_interval_{2000};
  uint32_t last_hb_{0};

  // cleaning detection
  binary_sensor::BinarySensor *cleaning_{nullptr};
  uint8_t battery_dp_{50};      // DP50 = [charge_state][battery%]
  uint8_t activity_dp_{111};    // DP111 = fast run-counter while cleaning
  int activity_threshold_{16};  // min DP111 advance between polls to count as "moving"
  uint32_t cleaning_timeout_ms_{360000};  // 6 min with no activity -> not cleaning
  int last_battery_{-1};
  long last_activity_val_{-1};
  uint32_t last_activity_ms_{0};
  bool cleaning_state_{false};
  bool charging_{false};  // internal truth: suppresses cleaning + drives the charging sensor

  // charging detection
  binary_sensor::BinarySensor *charging_sensor_{nullptr};
  uint32_t charge_timeout_ms_{600000};  // 10 min with no battery rise (and not charge_state==1) -> not charging
  uint32_t last_charge_rise_ms_{0};
};

}  // namespace wybot_bridge
}  // namespace esphome
