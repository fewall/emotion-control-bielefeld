#pragma once

#include "esphome/core/component.h"
#include "esphome/components/spi/spi.h"

namespace esphome {
namespace cc2500_scanner {

// 250 channels x 333.25 kHz starting at 2400.0 MHz -> 2400.0 ... 2483.0 MHz,
// i.e. the whole 2.4 GHz ISM band the CC2500 may legally use.
static const uint8_t NUM_CHANNELS = 250;

class CC2500Scanner : public Component,
                      public spi::SPIDevice<spi::BIT_ORDER_MSB_FIRST, spi::CLOCK_POLARITY_LOW,
                                            spi::CLOCK_PHASE_LEADING, spi::DATA_RATE_1MHZ> {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  void set_report_interval(uint32_t ms) { this->report_interval_ms_ = ms; }
  void set_dwell_us(uint32_t us) { this->dwell_us_ = us; }

 protected:
  uint8_t strobe_(uint8_t cmd);
  void write_register_(uint8_t addr, uint8_t value);
  uint8_t read_register_(uint8_t addr);
  uint8_t read_status_(uint8_t addr);
  bool wait_state_(uint8_t state, uint32_t timeout_us);
  int8_t measure_channel_(uint8_t channel);
  float channel_mhz_(uint8_t channel) const;
  void report_();
  void log_narrow_peaks_(const int8_t *peaks, const char *title);

  uint32_t report_interval_ms_{5000};
  uint32_t dwell_us_{300};
  uint32_t last_report_ms_{0};
  uint32_t window_start_ms_{0};
  uint32_t sweeps_{0};
  uint32_t window_sweeps_{0};
  uint32_t timeouts_{0};
  uint8_t partnum_{0};
  uint8_t version_{0};
  uint8_t next_channel_{0};
  bool avg_ready_{false};

  int8_t window_peak_[NUM_CHANNELS];  // max RSSI since the last report
  int8_t hold_peak_[NUM_CHANNELS];    // max RSSI since boot
  float avg_[NUM_CHANNELS];           // slow running average = background level
};

}  // namespace cc2500_scanner
}  // namespace esphome
