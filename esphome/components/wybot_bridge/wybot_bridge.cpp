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

void WybotBridge::register_poll_(uint8_t dp, uint8_t tier) {
  // Keep the STRONGEST (lowest-numbered) tier a DP was requested at: e.g. DP50 fed by both a fast
  // numeric and a slow raw-text entry stays fast. The rings themselves are built in setup().
  auto it = this->tier_of_.find(dp);
  if (it == this->tier_of_.end() || tier < it->second)
    this->tier_of_[dp] = tier;
}

void WybotBridge::bump_fast_() {
  // After a control write, re-query the DPs the UI reacts to so it updates in well under a second
  // instead of waiting for the fast ring to come around.
  for (uint8_t dp : {(uint8_t) 0, (uint8_t) 1, this->activity_dp_})
    this->priority_.push_back(dp);
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
  // The derived cleaning/charging detectors depend on the battery + run-counter DPs, so force those
  // into the fast ring no matter what tier their entities asked for.
  this->tier_of_[this->battery_dp_] = 0;
  this->tier_of_[this->activity_dp_] = 0;
  // Build the three poll rings from the per-DP tiers collected during registration.
  this->fast_.clear();
  this->slow_.clear();
  this->static_.clear();
  for (auto &kv : this->tier_of_) {
    if (kv.second == 0)
      this->fast_.push_back(kv.first);
    else if (kv.second == 2)
      this->static_.push_back(kv.first);
    else
      this->slow_.push_back(kv.first);
  }
  for (uint8_t dp : this->static_)
    this->static_last_[dp] = 0;  // 0 = never polled -> due immediately
  ESP_LOGCONFIG(TAG, "WyBot bridge up: %u fast / %u slow / %u static DP(s), heartbeat %ums",
                (unsigned) this->fast_.size(), (unsigned) this->slow_.size(),
                (unsigned) this->static_.size(), (unsigned) this->hb_interval_);
  if (this->cleaning_ != nullptr)
    this->cleaning_->publish_state(false);  // definite "off" at boot, not unknown (self-corrects on first drain)
  if (this->charging_sensor_ != nullptr)
    this->charging_sensor_->publish_state(false);  // definite "off" at boot (self-corrects on first battery rise)
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

  // auto-clear "charging" if the battery stops rising for a while (covers reports going quiet)
  if (this->charging_ && (now - this->last_charge_rise_ms_ > this->charge_timeout_ms_))
    this->set_charging_(false);

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
  // 1) Anything bumped by a recent control write jumps the queue.
  if (!this->priority_.empty()) {
    uint8_t dp = this->priority_.front();
    this->priority_.erase(this->priority_.begin());
    this->query_dp(dp);
    return;
  }

  // 2) 4 of every 5 slots serve the fast ring; the 5th serves a due static DP (re-polled every
  //    static_interval), else the next slow DP. A non-responding DP never stalls a slot: we fire
  //    and advance regardless of whether a reply comes.
  const uint32_t now = millis();
  bool slow_slot = (this->poll_counter_++ % 5) == 4;

  if (!slow_slot && !this->fast_.empty()) {
    uint8_t dp = this->fast_[this->fast_idx_ % this->fast_.size()];
    this->fast_idx_++;
    this->query_dp(dp);
    return;
  }

  // slow slot (or fast ring empty): a static DP past its re-poll interval wins.
  for (auto &kv : this->static_last_) {
    if (now - kv.second >= this->static_interval_ms_) {
      kv.second = now;
      this->query_dp(kv.first);
      return;
    }
  }
  if (!this->slow_.empty()) {
    uint8_t dp = this->slow_[this->slow_idx_ % this->slow_.size()];
    this->slow_idx_++;
    this->query_dp(dp);
    return;
  }
  // nothing slow/static due -> use the slot for fast so the bus stays busy
  if (!this->fast_.empty()) {
    uint8_t dp = this->fast_[this->fast_idx_ % this->fast_.size()];
    this->fast_idx_++;
    this->query_dp(dp);
  }
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

    // Catch-all: if the device reports a DP that no entity (and neither derived detector) listens
    // for, log it ONCE with its type/len so nothing it sends goes unnoticed. This is the guarantee
    // that "every data point is accounted for" even before each one has an entity.
    if (!this->warned_.count(dp)) {
      bool listened = this->txt_.count(dp) || dp == this->battery_dp_ || dp == this->activity_dp_;
      for (auto &r : this->num_)
        if (r.dp == dp) { listened = true; break; }
      if (!listened) {
        this->warned_[dp] = true;
        ESP_LOGW(TAG, "unlistened DP%u (type=0x%02X len=%u) reported by MCU - no entity", (unsigned) dp,
                 type, (unsigned) len);
      }
    }

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

void WybotBridge::set_charging_(bool v) {
  if (v == this->charging_)
    return;
  this->charging_ = v;
  if (this->charging_sensor_ != nullptr)
    this->charging_sensor_->publish_state(v);
}

void WybotBridge::update_cleaning_(uint8_t dp, const std::vector<uint8_t> &data) {
  const uint32_t now = millis();

  // DP50 = [charge_state][battery%]. charge_state 1 = charging on the dock. Battery only drops
  // while the motors run, so a downward tick (when not charging) means the robot is cleaning.
  if (dp == this->battery_dp_ && data.size() >= 2) {
    int charge_state = data[0];  // 0 unplugged/solar, 1 charging (standard dock), 2 charged/full
    int batt = data[1];
    bool rising = (this->last_battery_ >= 0 && batt > this->last_battery_);
    bool draining = (this->last_battery_ >= 0 && batt < this->last_battery_);

    // Charging: explicit charge_state==1, OR the pack % ticked up (the solar-dock case, where
    // charge_state stays 0). Latch off when full/charged, when draining, or after charge_timeout.
    if (batt >= 100 || charge_state == 2) {
      this->set_charging_(false);
    } else if (charge_state == 1 || rising) {
      this->last_charge_rise_ms_ = now;
      this->set_charging_(true);
    } else if (draining) {
      this->set_charging_(false);
    }

    // Cleaning: battery drawn down while NOT charging means the motors are running.
    if (this->charging_) {
      this->set_cleaning_(false);
    } else if (draining) {
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
  ESP_LOGCONFIG(TAG, "WyBot bridge: AA5500 DP protocol over UART, %u fast / %u slow / %u static DP(s)",
                (unsigned) this->fast_.size(), (unsigned) this->slow_.size(),
                (unsigned) this->static_.size());
  this->check_uart_settings(115200);
}

}  // namespace wybot_bridge
}  // namespace esphome
