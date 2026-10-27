#pragma once

#include "esphome/core/component.h"
#include "esphome/components/spi/spi.h"

namespace esphome {
namespace cc2500_capture {

class CC2500Capture : public Component,
                      public spi::SPIDevice<spi::BIT_ORDER_MSB_FIRST, spi::CLOCK_POLARITY_LOW,
                                            spi::CLOCK_PHASE_LEADING, spi::DATA_RATE_1MHZ> {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  void set_frequency_hz(float hz) { this->frequency_hz_ = hz; }
  void set_fixed_profile(int profile) { this->fixed_profile_ = profile; }
  void set_profile_time(uint32_t ms) { this->profile_time_ms_ = ms; }

 protected:
  uint8_t strobe_(uint8_t cmd);
  void write_register_(uint8_t addr, uint8_t value);
  uint8_t read_register_(uint8_t addr);
  uint8_t read_status_(uint8_t addr);
  void read_fifo_(uint8_t *buf, uint8_t len);
  void apply_profile_(int index);
  void restart_rx_();

  float frequency_hz_{2433.0e6f};
  int fixed_profile_{-1};
  uint32_t profile_time_ms_{20000};

  int profile_{0};
  uint32_t profile_start_ms_{0};
  uint32_t captures_{0};
  uint32_t suppressed_{0};
  uint32_t log_window_ms_{0};
  uint8_t logs_in_window_{0};
  uint8_t partnum_{0};
  uint8_t version_{0};
};

}  // namespace cc2500_capture
}  // namespace esphome
