#pragma once

#include <string>
#include <vector>

#include "esphome/core/component.h"
#include "esphome/core/preferences.h"
#include "esphome/components/spi/spi.h"
#include "esphome/components/button/button.h"
#include "esphome/components/light/light_output.h"
#include "esphome/components/light/light_state.h"
#include "esphome/components/number/number.h"
#include "esphome/components/text_sensor/text_sensor.h"

namespace esphome {
namespace ls_bielefeld {

// One frame on air: [len=0x0E][0x09][key][counter hi][counter lo][0x00][seq][remote ID x8] + HW CRC.
class LSChannelLight;
class LSColorNumber;

// Estimated state of one lamp channel. The control gives no feedback, so this is a model
// fed by everything the ESP sends and everything it hears from the real remote.
struct ChannelModel {
  bool on{false};
  float level{1.0f};    // 0 = minimum brightness, 1 = maximum
  bool next_up{false};  // direction of the next dimming hold (alternates with every hold)
};

struct PendingTarget {
  bool active{false};
  bool on{false};
  float value{0.0f};
  uint32_t since_ms{0};
};

struct TxFrame {
  uint8_t key;
  uint16_t counter;
  uint8_t seq;
  uint32_t not_before_ms;
};

class LSBielefeld : public Component,
                    public spi::SPIDevice<spi::BIT_ORDER_MSB_FIRST, spi::CLOCK_POLARITY_LOW,
                                          spi::CLOCK_PHASE_LEADING, spi::DATA_RATE_1MHZ> {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  void set_remote_id(uint8_t b0, uint8_t b1, uint8_t b2, uint8_t b3, uint8_t b4, uint8_t b5, uint8_t b6,
                     uint8_t b7) {
    const uint8_t id[8] = {b0, b1, b2, b3, b4, b5, b6, b7};
    for (int i = 0; i < 8; i++)
      this->remote_id_[i] = id[i];
    this->has_remote_id_ = true;
  }
  void set_frequency_hz(float hz) { this->frequency_hz_ = hz; }
  void set_deviation_hz(float hz) { this->deviation_hz_ = hz; }
  void set_repeats(uint8_t n) { this->repeats_ = n; }
  void set_key_sensor(text_sensor::TextSensor *s) { this->key_sensor_ = s; }
  void set_dim_time_ms(uint32_t ms) { this->dim_time_ms_ = ms; }
  void set_color_time_ms(uint32_t ms) { this->color_time_ms_ = ms; }
  void set_press_gap_ms(uint32_t ms) { this->press_gap_ms_ = ms; }
  void set_color_number(LSColorNumber *n) { this->color_number_ = n; }

  /// Emulate a key press on the remote. hold_ms = 0 is a short press.
  /// update_lights = false is used by the lights themselves, which already know the new state.
  void press(uint8_t key, uint32_t hold_ms, bool update_lights = true);
  void register_light(uint8_t channel, LSChannelLight *light) {
    if (channel >= 1 && channel <= 4)
      this->lights_[channel - 1] = light;
  }

  /// Desired state of a channel (from its light entity). Executed after a short debounce.
  void request_channel(uint8_t channel, bool on, float brightness);
  /// State restored at boot: taken over as the model, nothing is sent.
  void adopt_channel(uint8_t channel, bool on, float brightness);
  /// Desired colour temperature, 0 = coldest, 1 = warmest (from the number entity).
  void request_color(float warmth);

  static std::string key_name(uint8_t key);

 protected:
  uint8_t strobe_(uint8_t cmd);
  void write_register_(uint8_t addr, uint8_t value);
  uint8_t read_register_(uint8_t addr);
  uint8_t read_status_(uint8_t addr);
  void write_burst_(uint8_t addr, const uint8_t *data, uint8_t len);
  void read_fifo_(uint8_t *buf, uint8_t len);
  bool wait_state_(uint8_t state, uint32_t timeout_us);
  bool configure_radio_();
  void restart_rx_();
  void send_frame_(const TxFrame &frame);
  void poll_rx_();
  void handle_frame_(const uint8_t *payload, int rssi);
  void update_counter_(uint16_t seen);
  void publish_key_(const char *text);
  void apply_key_effect_(uint8_t key, uint32_t hold_ms);
  std::vector<uint32_t> plan_holds_(int index, float target) const;
  void apply_channel_targets_();
  void apply_color_target_(float warmth);
  void publish_models_();
  void finish_remote_press_();

  uint8_t remote_id_[8]{};
  bool has_remote_id_{false};
  float frequency_hz_{2433.0e6f};
  float deviation_hz_{38.0e3f};
  uint8_t repeats_{6};
  text_sensor::TextSensor *key_sensor_{nullptr};

  uint16_t counter_{0};
  bool counter_known_{false};
  ESPPreferenceObject pref_;

  std::vector<TxFrame> queue_;
  bool transmitting_{false};
  uint32_t tx_start_ms_{0};

  uint32_t rx_wait_since_ms_{0};
  bool rx_have_last_{false};
  uint8_t rx_last_key_{0};
  uint16_t rx_last_counter_{0};
  uint8_t rx_last_seq_{0};
  uint8_t rx_pressed_key_{0};
  bool rx_press_open_{false};
  bool rx_press_held_{false};
  uint32_t rx_last_frame_ms_{0};
  LSChannelLight *lights_[4]{nullptr, nullptr, nullptr, nullptr};
  uint32_t rx_press_start_ms_{0};
  uint32_t foreign_logged_ms_{0};
  uint32_t tx_free_at_ms_{0};

  uint32_t dim_time_ms_{1000};
  uint32_t color_time_ms_{1200};
  uint32_t press_gap_ms_{250};  // pause between two emulated presses
  ChannelModel channels_[4];
  PendingTarget pending_[4];
  float color_{1.0f};
  PendingTarget color_pending_;
  bool batch_{false};
  LSColorNumber *color_number_{nullptr};
  ESPPreferenceObject color_pref_;
  uint8_t partnum_{0};
  uint8_t version_{0};
};

class LSButton : public button::Button {
 public:
  void set_parent(LSBielefeld *parent) { this->parent_ = parent; }
  void set_key(uint8_t key) { this->key_ = key; }
  void set_hold_ms(uint32_t ms) { this->hold_ms_ = ms; }

 protected:
  void press_action() override {
    if (this->parent_ != nullptr)
      this->parent_->press(this->key_, this->hold_ms_);
  }
  LSBielefeld *parent_{nullptr};
  uint8_t key_{0};
  uint32_t hold_ms_{0};
};

class LSChannelLight : public light::LightOutput {
 public:
  void set_parent(LSBielefeld *parent) { this->parent_ = parent; }
  void set_channel(uint8_t channel) { this->channel_ = channel; }
  light::LightTraits get_traits() override {
    auto traits = light::LightTraits();
    traits.set_supported_color_modes({light::ColorMode::BRIGHTNESS});
    return traits;
  }
  void setup_state(light::LightState *state) override { this->state_ = state; }
  void write_state(light::LightState *state) override;

  /// Called by the hub when its model changed (remote, buttons, own commands):
  /// shows the new state without transmitting anything.
  void set_estimate(bool on, float level);

 protected:
  LSBielefeld *parent_{nullptr};
  uint8_t channel_{1};
  light::LightState *state_{nullptr};
  bool initialized_{false};
};

class LSColorNumber : public number::Number {
 public:
  void set_parent(LSBielefeld *parent) { this->parent_ = parent; }

 protected:
  void control(float value) override {
    if (this->parent_ != nullptr)
      this->parent_->request_color(value / 100.0f);
  }
  LSBielefeld *parent_{nullptr};
};

}  // namespace ls_bielefeld
}  // namespace esphome
