#include "wybot_bridge.h"

namespace esphome {
namespace wybot_bridge {

static const char *const TAG = "wybot_bridge";

uint8_t WybotBridge::checksum(const std::vector<uint8_t> &f, size_t upto) {
  uint32_t s = 0;
  for (size_t i = 0; i < upto && i < f.size(); i++)
    s += f[i];
  return (uint8_t) (s & 0xFF);
}

void WybotBridge::send_dp(uint16_t cmd, uint8_t dp_id, uint8_t dp_type, const std::vector<uint8_t> &data) {
  std::vector<uint8_t> payload;
  payload.push_back(dp_id);
  payload.push_back(dp_type);
  payload.push_back((uint8_t) (data.size() >> 8));   // dp_len big-endian
  payload.push_back((uint8_t) (data.size() & 0xFF));
  for (uint8_t b : data)
    payload.push_back(b);

  std::vector<uint8_t> f;
  f.push_back(HDR0);
  f.push_back(HDR1);
  f.push_back((uint8_t) (cmd >> 8));                  // cmd big-endian
  f.push_back((uint8_t) (cmd & 0xFF));
  f.push_back((uint8_t) (payload.size() & 0xFF));     // len little-endian
  f.push_back((uint8_t) (payload.size() >> 8));
  for (uint8_t b : payload)
    f.push_back(b);
  f.push_back(checksum(f, f.size()));

  this->write_array(f);
  ESP_LOGD(TAG, "TX cmd=0x%04X dp=%u type=%u len=%u", cmd, dp_id, dp_type, (unsigned) data.size());
}

void WybotBridge::loop() {
  while (this->available()) {
    uint8_t b;
    this->read_byte(&b);
    // resync on the AA 55 header
    if (this->rx_.empty() && b != HDR0)
      continue;
    if (this->rx_.size() == 1 && b != HDR1) {
      this->rx_.clear();
      if (b == HDR0)
        this->rx_.push_back(b);
      continue;
    }
    this->rx_.push_back(b);

    if (this->rx_.size() >= 6) {
      uint16_t plen = (uint16_t) this->rx_[4] | ((uint16_t) this->rx_[5] << 8);  // len little-endian
      size_t total = 6 + plen + 1;  // header(6) + payload + checksum
      if (plen > 512) {  // garbage guard
        this->rx_.clear();
        continue;
      }
      if (this->rx_.size() >= total) {
        std::vector<uint8_t> frame(this->rx_.begin(), this->rx_.begin() + total);
        this->rx_.erase(this->rx_.begin(), this->rx_.begin() + total);
        this->handle_frame(frame);
      }
    }
    if (this->rx_.size() > 1024)
      this->rx_.clear();
  }
}

void WybotBridge::handle_frame(const std::vector<uint8_t> &f) {
  uint16_t cmd = ((uint16_t) f[2] << 8) | f[3];
  uint8_t want = checksum(f, f.size() - 1);
  bool ck_ok = (want == f.back());
  if (f.size() >= 10) {
    uint8_t dp_id = f[6], dp_type = f[7];
    ESP_LOGD(TAG, "RX cmd=0x%04X dp=%u type=%u ck=%s", cmd, dp_id, dp_type, ck_ok ? "ok" : "BAD");
  }
  if (this->status_ != nullptr) {
    char buf[32];
    snprintf(buf, sizeof(buf), "cmd=0x%04X len=%u", cmd, (unsigned) f.size());
    this->status_->publish_state(buf);
  }
}

void WybotBridge::dump_config() {
  ESP_LOGCONFIG(TAG, "WyBot bridge (DP over UART; framing to confirm against MCU capture)");
  this->check_uart_settings(0);  // baud is set in the uart: block
}

}  // namespace wybot_bridge
}  // namespace esphome
