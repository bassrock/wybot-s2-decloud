#include "wybot_pinscan.h"
#include <driver/gpio.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_system.h>
#include <inttypes.h>

namespace esphome {
namespace wybot_pinscan {

static const char *const TAG = "wybot_pinscan";

void WybotPinScan::setup() {
  if (this->pins_.empty())
    this->pins_ = {0, 1, 3, 4, 5, 6, 7, 10};
  if (this->bauds_.empty())
    this->bauds_ = {115200};

  uart_config_t cfg = {};
  cfg.baud_rate = this->bauds_[0];
  cfg.data_bits = UART_DATA_8_BITS;
  cfg.parity = UART_PARITY_DISABLE;
  cfg.stop_bits = UART_STOP_BITS_1;
  cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
  cfg.source_clk = UART_SCLK_DEFAULT;

  esp_err_t e1 = uart_driver_install(this->uart_num_, 2048, 0, 0, nullptr, 0);
  esp_err_t e2 = uart_param_config(this->uart_num_, &cfg);
  this->installed_ = (e1 == ESP_OK && e2 == ESP_OK);
  ESP_LOGI(TAG, "ready on UART%d (install=0x%x cfg=0x%x); %u candidate pins x %u bauds, %ums dwell",
           (int) this->uart_num_, e1, e2, (unsigned) this->pins_.size(),
           (unsigned) this->bauds_.size(), (unsigned) this->dwell_);
  ESP_LOGI(TAG, "listening PASSIVELY (RX only, TX never driven). Watch for '** HIT **' lines.");
  ESP_LOGI(TAG, "probing is DISARMED until the 'Pin scan enabled' switch is on AND an API client "
                "is connected -- no GPIO is touched before then.");
  this->log_partitions();
}

void WybotPinScan::log_partitions() {
  const esp_partition_t *run = esp_ota_get_running_partition();
  const esp_partition_t *next = esp_ota_get_next_update_partition(nullptr);
  if (run != nullptr)
    ESP_LOGW(TAG, "running slot: %-8s @0x%06" PRIx32 " size=0x%06" PRIx32, run->label, run->address,
             run->size);
  if (next != nullptr) {
    ESP_LOGW(TAG, "next OTA slot: %-8s @0x%06" PRIx32 " size=0x%06" PRIx32, next->label,
             next->address, next->size);
  } else {
    ESP_LOGE(TAG, "next OTA slot: <<NONE>> -- single-OTA layout; stage-2 ESPHome OTA may fail. "
                  "Keep the stock slot intact and use boot_other_slot to revert.");
  }
  esp_partition_iterator_t it =
      esp_partition_find(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, nullptr);
  while (it != nullptr) {
    const esp_partition_t *p = esp_partition_get(it);
    ESP_LOGW(TAG, "  app part: %-8s subtype=0x%02x @0x%06" PRIx32 " size=0x%06" PRIx32, p->label,
             p->subtype, p->address, p->size);
    it = esp_partition_next(it);
  }
}

void WybotPinScan::boot_other_slot() {
  const esp_partition_t *other = esp_ota_get_next_update_partition(nullptr);
  if (other == nullptr) {
    ESP_LOGE(TAG, "boot_other_slot: no other app slot; cannot switch");
    return;
  }
  esp_err_t e = esp_ota_set_boot_partition(other);
  ESP_LOGW(TAG, "boot_other_slot: set boot -> %s (err=0x%x); rebooting in 1s", other->label, e);
  if (e == ESP_OK)
    esp_restart();
}

void WybotPinScan::loop() {
  if (!this->installed_)
    return;

  // Safety gate: only probe when explicitly armed AND actively reachable. On a sealed dock this
  // guarantees the device first comes up on WiFi and is being watched before any pin is touched,
  // and that scanning stops the moment the operator disconnects.
  bool gated = !this->enabled_;
#ifdef USE_WIFI
  if (wifi::global_wifi_component == nullptr || !wifi::global_wifi_component->is_connected())
    gated = true;
#endif
#ifdef USE_API
  if (api::global_api_server == nullptr || !api::global_api_server->is_connected())
    gated = true;
#endif
  if (gated) {
    if (!this->was_gated_) {
      ESP_LOGW(TAG, "probing PAUSED (disarmed or not reachable); no GPIO is being touched");
      this->was_gated_ = true;
    }
    return;
  }
  if (this->was_gated_) {
    ESP_LOGW(TAG, "probing ARMED and reachable; starting passive RX scan on UART%d",
             (int) this->uart_num_);
    this->was_gated_ = false;
    this->last_switch_ = 0;  // force an immediate window switch
    this->bi_ = -1;
  }

  const uint32_t now = millis();
  if (this->bi_ >= 0 && (now - this->last_switch_) < this->dwell_) {
    this->scan_window_();
    return;
  }
  // Summarize the window we just finished, then move to the next (pin, baud).
  if (this->bi_ >= 0) {
    // A line the GD32 drives idles HIGH (UART mark) even when no bytes flow, so the idle level is
    // a second, byte-independent tell for "this pin is a live UART TX from the MCU".
    int idle = gpio_get_level((gpio_num_t) this->pins_[this->pi_]);
    if (this->aa55_ > 0) {
      ESP_LOGW(TAG, "** HIT ** GPIO%d @ %d baud: %u bytes, %u AA55 frame-starts, idle=%d -> C3 RX",
               this->pins_[this->pi_], this->bauds_[this->bi_], (unsigned) this->bytes_seen_,
               (unsigned) this->aa55_, idle);
    } else {
      ESP_LOGI(TAG, "   GPIO%d @ %d baud: %u bytes, no AA55, idle=%d", this->pins_[this->pi_],
               this->bauds_[this->bi_], (unsigned) this->bytes_seen_, idle);
    }
  }
  this->advance_();
  this->last_switch_ = now;
  this->bytes_seen_ = 0;
  this->aa55_ = 0;
  const int pin = this->pins_[this->pi_];
  const int baud = this->bauds_[this->bi_];
  uart_set_baudrate(this->uart_num_, baud);
  // RX only: tx/rts/cts unchanged (never routed), rx = candidate pin.
  esp_err_t e = uart_set_pin(this->uart_num_, UART_PIN_NO_CHANGE, pin, UART_PIN_NO_CHANGE,
                             UART_PIN_NO_CHANGE);
  uart_flush_input(this->uart_num_);
  ESP_LOGD(TAG, ">> now listening RX=GPIO%d @ %d baud (set_pin=0x%x)", pin, baud, e);
}

void WybotPinScan::scan_window_() {
  uint8_t buf[256];
  int n = uart_read_bytes(this->uart_num_, buf, sizeof(buf), 0);
  if (n <= 0)
    return;
  this->bytes_seen_ += n;
  for (int i = 0; i + 1 < n; i++)
    if (buf[i] == 0xAA && buf[i + 1] == 0x55)
      this->aa55_++;
  // Hex-dump up to the first 32 bytes so real framing / DP ids are captured in the log.
  char hex[32 * 3 + 1];
  int m = n < 32 ? n : 32, o = 0;
  for (int i = 0; i < m; i++)
    o += snprintf(hex + o, 4, "%02X ", buf[i]);
  ESP_LOGI(TAG, "   GPIO%d @%d rx[%d]: %s", this->pins_[this->pi_], this->bauds_[this->bi_], n, hex);
}

void WybotPinScan::advance_() {
  this->bi_++;
  if (this->bi_ >= (int) this->bauds_.size()) {
    this->bi_ = 0;
    this->pi_ = (this->pi_ + 1) % (int) this->pins_.size();
  }
}

void WybotPinScan::dump_config() {
  ESP_LOGCONFIG(TAG, "WyBot pin scanner: UART%d, %u pins, %u bauds, %ums dwell",
                (int) this->uart_num_, (unsigned) this->pins_.size(),
                (unsigned) this->bauds_.size(), (unsigned) this->dwell_);
}

}  // namespace wybot_pinscan
}  // namespace esphome
