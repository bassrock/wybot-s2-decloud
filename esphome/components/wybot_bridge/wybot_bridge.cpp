#include "wybot_bridge.h"

namespace esphome {
namespace wybot_bridge {

static const char *const TAG = "wybot_bridge";

uint8_t WybotBridge::checksum_(const std::vector<uint8_t> &f, size_t upto) {
  uint32_t s = 0;
  for (size_t i = 0; i < upto && i < f.size(); i++)
    s += f[i];
  return (uint8_t) (s & 0xFF);
}

std::vector<uint8_t> WybotBridge::dp_frame_(uint8_t dp, uint8_t type, const std::vector<uint8_t> &data) {
  std::vector<uint8_t> p;
  p.push_back(dp);
  p.push_back(type);
  p.push_back((uint8_t) data.size());
  for (uint8_t b : data)
    p.push_back(b);
  return p;
}

void WybotBridge::register_poll_(uint8_t dp) {
  for (uint8_t d : this->poll_list_)
    if (d == dp)
      return;
  this->poll_list_.push_back(dp);
}

void WybotBridge::send_cmd(uint8_t cmd, const std::vector<uint8_t> &payload) {
  std::vector<uint8_t> f;
  f.push_back(0xAA);
  f.push_back(0x55);
  f.push_back(0x00);
  f.push_back(cmd);
  f.push_back((uint8_t) (payload.size() & 0xFF));         // plen little-endian
  f.push_back((uint8_t) ((payload.size() >> 8) & 0xFF));
  for (uint8_t b : payload)
    f.push_back(b);
  f.push_back(checksum_(f, f.size()));
  this->write_array(f);
}

void WybotBridge::setup() {
  ESP_LOGCONFIG(TAG, "WyBot bridge up: %u DP(s) polled, heartbeat %ums", (unsigned) this->poll_list_.size(),
                (unsigned) this->hb_interval_);
  if (this->cleaning_ != nullptr)
    this->cleaning_->publish_state(false);  // definite "off" at boot, not unknown (self-corrects on first drain)
  this->heartbeat();  // kick the slave once at boot
}

void WybotBridge::loop() {
  // keep-alive heartbeat
  const uint32_t now = millis();
  if (this->hb_interval_ > 0 && now - this->last_hb_ >= this->hb_interval_) {
    this->last_hb_ = now;
    this->heartbeat();
  }

  // auto-clear "cleaning" once the robot stops drawing the battery down (or starts charging)
  if (this->cleaning_state_ && (this->charging_ || (now - this->last_activity_ms_ > this->cleaning_timeout_ms_)))
    this->set_cleaning_(false);

  // drain + frame UART
  while (this->available()) {
    uint8_t b;
    this->read_byte(&b);
    if (this->rx_.empty() && b != 0xAA)
      continue;
    if (this->rx_.size() == 1 && b != 0x55) {
      this->rx_.clear();
      if (b == 0xAA)
        this->rx_.push_back(b);
      continue;
    }
    if (this->rx_.size() == 2 && b != 0x00) {  // 3rd header byte must be 0x00
      this->rx_.clear();
      if (b == 0xAA)
        this->rx_.push_back(b);
      continue;
    }
    this->rx_.push_back(b);

    if (this->rx_.size() >= 6) {
      uint16_t plen = (uint16_t) this->rx_[4] | ((uint16_t) this->rx_[5] << 8);
      if (plen > 512) {  // garbage guard
        this->rx_.clear();
        continue;
      }
      size_t total = 6 + plen + 1;
      if (this->rx_.size() >= total) {
        std::vector<uint8_t> frame(this->rx_.begin(), this->rx_.begin() + total);
        this->rx_.erase(this->rx_.begin(), this->rx_.begin() + total);
        this->handle_frame_(frame);
      }
    }
    if (this->rx_.size() > 1024)
      this->rx_.clear();
  }
}

void WybotBridge::update() {
  if (this->poll_list_.empty())
    return;
  uint8_t dp = this->poll_list_[this->poll_idx_];
  this->poll_idx_ = (this->poll_idx_ + 1) % this->poll_list_.size();
  this->query_dp(dp);
}

void WybotBridge::handle_frame_(const std::vector<uint8_t> &f) {
  uint8_t cmd = f[3];
  uint8_t want = checksum_(f, f.size() - 1);
  if (want != f.back()) {
    ESP_LOGW(TAG, "bad checksum on cmd=0x%02X (len=%u)", cmd, (unsigned) f.size());
    return;
  }
  uint16_t plen = (uint16_t) f[4] | ((uint16_t) f[5] << 8);
  std::vector<uint8_t> payload(f.begin() + 6, f.begin() + 6 + plen);
  if (cmd == 0x05)
    this->handle_report_(payload);
}

void WybotBridge::handle_report_(const std::vector<uint8_t> &p) {
  size_t j = 0;
  while (j + 3 <= p.size()) {
    uint8_t dp = p[j], type = p[j + 1], len = p[j + 2];
    if (j + 3 + len > p.size())
      break;
    std::vector<uint8_t> data(p.begin() + j + 3, p.begin() + j + 3 + len);
    j += 3 + len;

    this->last_data_[dp] = data;  // cache for template/select/button lambdas
    this->update_cleaning_(dp, data);

    for (auto &r : this->num_) {
      if (r.dp != dp)
        continue;
      float v = 0;
      int off = r.offset, w = r.width;
      if (off < 0 || w < 0) {  // whole value: full LE int, signed by type or flag
        off = 0;
        w = (int) data.size();
      }
      int32_t iv = 0;
      for (int k = 0; k < w && (size_t) (off + k) < data.size() && k < 4; k++)
        iv |= ((int32_t) data[off + k]) << (8 * k);
      bool sgn = r.is_signed || (type == 0x02 && (r.offset < 0));
      if (sgn && w >= 1 && w <= 4) {
        int bits = 8 * w;
        if (iv & (1 << (bits - 1)))
          iv |= ~((1 << bits) - 1);
      }
      r.s->publish_state((float) iv * r.scale);
    }

    auto itt = this->txt_.find(dp);
    if (itt != this->txt_.end()) {
      char buf[48];
      if (itt->second.hex) {
        size_t o = 0;
        for (size_t k = 0; k < data.size() && o + 3 < sizeof(buf); k++)
          o += snprintf(buf + o, sizeof(buf) - o, "%02X", data[k]);
        if (data.empty())
          snprintf(buf, sizeof(buf), "(empty)");
      } else {
        uint32_t uv = 0;
        for (size_t k = 0; k < data.size() && k < 4; k++)
          uv |= ((uint32_t) data[k]) << (8 * k);
        snprintf(buf, sizeof(buf), "%u", (unsigned) uv);
      }
      itt->second.s->publish_state(buf);
    }
  }
}

void WybotBridge::set_cleaning_(bool v) {
  if (v == this->cleaning_state_)
    return;
  this->cleaning_state_ = v;
  if (this->cleaning_ != nullptr)
    this->cleaning_->publish_state(v);
}

void WybotBridge::update_cleaning_(uint8_t dp, const std::vector<uint8_t> &data) {
  const uint32_t now = millis();

  // DP50 = [charge_state][battery%]. charge_state 1 = charging on the dock. Battery only drops
  // while the motors run, so a downward tick (when not charging) means the robot is cleaning.
  if (dp == this->battery_dp_ && data.size() >= 2) {
    int charge_state = data[0];
    int batt = data[1];
    this->charging_ = (charge_state == 1);
    if (this->charging_) {
      this->set_cleaning_(false);
    } else if (this->last_battery_ >= 0 && batt < this->last_battery_) {
      this->last_activity_ms_ = now;
      this->set_cleaning_(true);
    }
    this->last_battery_ = batt;
  }

  // DP111 = a run-counter that climbs fast while cleaning and barely moves at rest. A large advance
  // between polls is a second, faster "robot is moving" signal (never trusted while charging).
  if (dp == this->activity_dp_ && !data.empty()) {
    long v = 0;
    for (size_t k = 0; k < data.size() && k < 4; k++)
      v |= ((long) data[k]) << (8 * k);
    if (this->last_activity_val_ >= 0 && !this->charging_ &&
        (v - this->last_activity_val_) >= this->activity_threshold_) {
      this->last_activity_ms_ = now;
      this->set_cleaning_(true);
    }
    this->last_activity_val_ = v;
  }
}

void WybotBridge::dump_config() {
  ESP_LOGCONFIG(TAG, "WyBot bridge: AA5500 DP protocol over UART, %u polled DP(s)",
                (unsigned) this->poll_list_.size());
  this->check_uart_settings(115200);
}

}  // namespace wybot_bridge
}  // namespace esphome
