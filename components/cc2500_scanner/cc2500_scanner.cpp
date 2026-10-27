#include "cc2500_scanner.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include <cinttypes>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace esphome {
namespace cc2500_scanner {

static const char *const TAG = "cc2500_scanner";

// SPI header bits
static const uint8_t READ_SINGLE = 0x80;
static const uint8_t BURST = 0x40;

// Command strobes
static const uint8_t SRES = 0x30;
static const uint8_t SRX = 0x34;
static const uint8_t SIDLE = 0x36;
static const uint8_t SFRX = 0x3A;

// Status registers (must be read with READ_SINGLE | BURST)
static const uint8_t PARTNUM = 0x30;
static const uint8_t VERSION = 0x31;
static const uint8_t RSSI = 0x34;
static const uint8_t MARCSTATE = 0x35;

static const uint8_t MARC_IDLE = 0x01;
static const uint8_t MARC_RX = 0x0D;

static const uint8_t REG_CHANNR = 0x0A;

// RSSI offset for the CC2500 (datasheet, table "RSSI offset"): ~72 dB at 250 kBaud.
static const int RSSI_OFFSET = 72;

// Base frequency, channel spacing (26 MHz crystal)
static const float BASE_MHZ = 2400.0f;     // FREQ = 0x5C4EC5
static const float SPACING_MHZ = 0.33325f;  // CHANSPC_E=3, CHANSPC_M=164

struct RegSetting {
  uint8_t addr;
  uint8_t value;
};

// Wide-open receiver used only for RSSI measurement. The demodulator settings
// do not matter for a spectrum scan; what matters is frequency, channel
// spacing and the widest possible receive filter (812 kHz), so a transmitter
// is seen even if it sits between two scan channels.
static const RegSetting CONFIG[] = {
    {0x00, 0x29},  // IOCFG2   CHIP_RDYn
    {0x02, 0x06},  // IOCFG0   sync word (unused here)
    {0x03, 0x07},  // FIFOTHR
    {0x04, 0xD3},  // SYNC1
    {0x05, 0x91},  // SYNC0
    {0x06, 0x3D},  // PKTLEN
    {0x07, 0x04},  // PKTCTRL1
    {0x08, 0x05},  // PKTCTRL0
    {0x09, 0x00},  // ADDR
    {0x0A, 0x00},  // CHANNR
    {0x0B, 0x0C},  // FSCTRL1  IF ~305 kHz
    {0x0C, 0x00},  // FSCTRL0
    {0x0D, 0x5C},  // FREQ2    2400.0 MHz
    {0x0E, 0x4E},  // FREQ1
    {0x0F, 0xC5},  // FREQ0
    {0x10, 0x0D},  // MDMCFG4  RX filter 812 kHz, DRATE_E=13
    {0x11, 0x3B},  // MDMCFG3  250 kBaud
    {0x12, 0x73},  // MDMCFG2  MSK, 30/32 sync
    {0x13, 0x23},  // MDMCFG1  CHANSPC_E=3
    {0x14, 0xA4},  // MDMCFG0  CHANSPC_M=164 -> 333.25 kHz
    {0x15, 0x00},  // DEVIATN
    {0x16, 0x07},  // MCSM2
    {0x17, 0x30},  // MCSM1
    {0x18, 0x18},  // MCSM0    auto-calibrate on IDLE->RX
    {0x19, 0x1D},  // FOCCFG
    {0x1A, 0x1C},  // BSCFG
    {0x1B, 0xC7},  // AGCCTRL2
    {0x1C, 0x00},  // AGCCTRL1
    {0x1D, 0xB0},  // AGCCTRL0
    {0x21, 0xB6},  // FREND1
    {0x22, 0x10},  // FREND0
    {0x23, 0xEA},  // FSCAL3
    {0x24, 0x0A},  // FSCAL2
    {0x25, 0x00},  // FSCAL1
    {0x26, 0x11},  // FSCAL0
    {0x29, 0x59},  // FSTEST
    {0x2C, 0x88},  // TEST2
    {0x2D, 0x31},  // TEST1
    {0x2E, 0x0B},  // TEST0
};

uint8_t CC2500Scanner::strobe_(uint8_t cmd) {
  this->enable();
  const uint8_t status = this->transfer_byte(cmd);
  this->disable();
  return status;
}

void CC2500Scanner::write_register_(uint8_t addr, uint8_t value) {
  this->enable();
  this->transfer_byte(addr);
  this->transfer_byte(value);
  this->disable();
}

uint8_t CC2500Scanner::read_register_(uint8_t addr) {
  this->enable();
  this->transfer_byte(addr | READ_SINGLE);
  const uint8_t value = this->transfer_byte(0x00);
  this->disable();
  return value;
}

uint8_t CC2500Scanner::read_status_(uint8_t addr) {
  this->enable();
  this->transfer_byte(addr | READ_SINGLE | BURST);
  const uint8_t value = this->transfer_byte(0x00);
  this->disable();
  return value;
}

bool CC2500Scanner::wait_state_(uint8_t state, uint32_t timeout_us) {
  const uint32_t start = micros();
  while ((this->read_status_(MARCSTATE) & 0x1F) != state) {
    if (micros() - start > timeout_us)
      return false;
  }
  return true;
}

float CC2500Scanner::channel_mhz_(uint8_t channel) const { return BASE_MHZ + channel * SPACING_MHZ; }

void CC2500Scanner::setup() {
  ESP_LOGCONFIG(TAG, "Setting up CC2500 spectrum scanner...");
  this->spi_setup();

  // Manual reset (datasheet 19.1.2)
  this->enable();
  delayMicroseconds(1);
  this->disable();
  delayMicroseconds(41);
  this->strobe_(SRES);
  delay(1);

  this->partnum_ = this->read_status_(PARTNUM);
  this->version_ = this->read_status_(VERSION);
  if (this->version_ == 0x00 || this->version_ == 0xFF) {
    ESP_LOGE(TAG, "CC2500 not responding (VERSION=0x%02X). Check wiring.", this->version_);
    this->mark_failed();
    return;
  }

  for (const auto &reg : CONFIG)
    this->write_register_(reg.addr, reg.value);
  for (const auto &reg : CONFIG) {
    const uint8_t got = this->read_register_(reg.addr);
    if (got != reg.value) {
      ESP_LOGE(TAG, "Register 0x%02X: wrote 0x%02X, read 0x%02X", reg.addr, reg.value, got);
      this->mark_failed();
      return;
    }
  }

  for (uint8_t i = 0; i < NUM_CHANNELS; i++) {
    this->window_peak_[i] = -128;
    this->hold_peak_[i] = -128;
    this->avg_[i] = 0.0f;
  }
  this->last_report_ms_ = millis();
  this->window_start_ms_ = this->last_report_ms_;
  ESP_LOGCONFIG(TAG, "Scanner ready. Press buttons on the remote and watch the log.");
}

int8_t CC2500Scanner::measure_channel_(uint8_t channel) {
  this->strobe_(SIDLE);
  this->wait_state_(MARC_IDLE, 1000);
  this->write_register_(REG_CHANNR, channel);
  this->strobe_(SFRX);
  this->strobe_(SRX);  // MCSM0 calibrates the synthesizer here (~800 us)
  if (!this->wait_state_(MARC_RX, 3000)) {
    this->timeouts_++;
    return -128;
  }
  delayMicroseconds(80);  // let AGC / RSSI settle on the new channel

  int best = -128;
  const uint32_t start = micros();
  do {
    const int raw = static_cast<int8_t>(this->read_status_(RSSI));
    const int dbm = raw / 2 - RSSI_OFFSET;
    if (dbm > best)
      best = dbm;
  } while (micros() - start < this->dwell_us_);
  return static_cast<int8_t>(best);
}

void CC2500Scanner::loop() {
  if (this->is_failed())
    return;

  // Work in slices of ~12 ms so the rest of ESPHome (Wi-Fi, API, logger)
  // keeps running. A full sweep over 250 channels takes ~0.3 s.
  const uint32_t slice_start = micros();
  while (micros() - slice_start < 12000) {
    const uint8_t ch = this->next_channel_;
    const int8_t dbm = this->measure_channel_(ch);

    if (dbm > this->window_peak_[ch])
      this->window_peak_[ch] = dbm;
    if (dbm > this->hold_peak_[ch])
      this->hold_peak_[ch] = dbm;
    if (!this->avg_ready_) {
      this->avg_[ch] = dbm;
    } else {
      this->avg_[ch] += (dbm - this->avg_[ch]) / 32.0f;
    }

    if (++this->next_channel_ >= NUM_CHANNELS) {
      this->next_channel_ = 0;
      this->sweeps_++;
      this->window_sweeps_++;
      this->avg_ready_ = true;
      if (millis() - this->last_report_ms_ >= this->report_interval_ms_) {
        this->report_();
        this->last_report_ms_ = millis();
      }
      return;
    }
  }
}

// Finds narrow-band signals: channels whose peak rises clearly above both
// their own long-term average and their neighbourhood. Wi-Fi is ~20 MHz wide
// (~60 scan channels) and therefore cancels out against its neighbours;
// a remote control occupies only one to three channels.
void CC2500Scanner::log_narrow_peaks_(const int8_t *peaks, const char *title) {
  float score[NUM_CHANNELS];
  float delta[NUM_CHANNELS];
  for (int i = 0; i < NUM_CHANNELS; i++)
    delta[i] = peaks[i] <= -128 ? 0.0f : peaks[i] - this->avg_[i];
  // Compare against the left and the right neighbourhood separately and use the
  // louder side, so the edge of a broad Wi-Fi block does not look "narrow".
  for (int i = 0; i < NUM_CHANNELS; i++) {
    float side_mean[2] = {0.0f, 0.0f};
    for (int side = 0; side < 2; side++) {
      float sum = 0;
      int n = 0;
      for (int d = 3; d <= 15; d++) {
        const int j = side == 0 ? i - d : i + d;
        if (j < 0 || j >= NUM_CHANNELS)
          continue;
        sum += delta[j];
        n++;
      }
      side_mean[side] = n > 0 ? sum / n : 0.0f;
    }
    score[i] = delta[i] - std::max(side_mean[0], side_mean[1]);
  }

  bool used[NUM_CHANNELS];
  memset(used, 0, sizeof(used));
  int found = 0;
  for (int k = 0; k < 5; k++) {
    int best = -1;
    for (int i = 0; i < NUM_CHANNELS; i++) {
      if (!used[i] && (best < 0 || score[i] > score[best]))
        best = i;
    }
    if (best < 0 || score[best] < 10.0f)
      break;
    for (int j = best - 3; j <= best + 3; j++)
      if (j >= 0 && j < NUM_CHANNELS)
        used[j] = true;
    if (found == 0)
      ESP_LOGI(TAG, "  %s:", title);
    ESP_LOGI(TAG, "    %.2f MHz (ch %3d): peak %4d dBm, background %4.0f dBm, +%.0f dB above surroundings",
             this->channel_mhz_(best), best, peaks[best], this->avg_[best], score[best]);
    found++;
  }
  if (found == 0)
    ESP_LOGI(TAG, "  %s: none", title);
}

void CC2500Scanner::report_() {
  static const char LEVELS[] = " .:-=+*#%@";
  const int cols = (NUM_CHANNELS + 2) / 3;  // 3 channels ~ 1 MHz per column
  char line[96];
  char axis[96];
  memset(axis, ' ', sizeof(axis));

  for (int c = 0; c < cols; c++) {
    int peak = -128;
    for (int k = 0; k < 3; k++) {
      const int ch = c * 3 + k;
      if (ch < NUM_CHANNELS && this->window_peak_[ch] > peak)
        peak = this->window_peak_[ch];
    }
    int level = (peak + 100) / 7;  // -100 dBm .. -37 dBm mapped onto 10 steps
    if (level < 0)
      level = 0;
    if (level > 9)
      level = 9;
    line[c] = LEVELS[level];
  }
  line[cols] = '\0';

  for (int mhz = 2400; mhz <= 2480; mhz += 10) {
    const int col = static_cast<int>(lroundf((mhz - BASE_MHZ) / (3 * SPACING_MHZ)));
    if (col + 4 <= cols)
      snprintf(axis + col, 5, "%d", mhz), axis[col + 4] = ' ';
  }
  axis[cols] = '\0';

  const uint32_t secs = (millis() - this->window_start_ms_) / 1000;
  ESP_LOGI(TAG, "--- %" PRIu32 " sweeps in the last %" PRIu32 " s (total %" PRIu32 ", timeouts %" PRIu32 ") ---", this->window_sweeps_, secs,
           this->sweeps_, this->timeouts_);
  ESP_LOGI(TAG, "  MHz |%s|", axis);
  ESP_LOGI(TAG, "  pk  |%s|", line);
  this->log_narrow_peaks_(this->window_peak_, "narrow peaks in this window");
  this->log_narrow_peaks_(this->hold_peak_, "strongest narrow peaks since boot");

  for (int i = 0; i < NUM_CHANNELS; i++)
    this->window_peak_[i] = -128;
  this->window_sweeps_ = 0;
  this->window_start_ms_ = millis();
}

void CC2500Scanner::dump_config() {
  ESP_LOGCONFIG(TAG, "CC2500 spectrum scanner:");
  ESP_LOGCONFIG(TAG, "  CC2500 PARTNUM=0x%02X VERSION=0x%02X", this->partnum_, this->version_);
  LOG_PIN("  CS Pin: ", this->cs_);
  ESP_LOGCONFIG(TAG, "  Range: %.2f - %.2f MHz, %u channels, spacing %.0f kHz, RX filter 812 kHz", BASE_MHZ,
                this->channel_mhz_(NUM_CHANNELS - 1), NUM_CHANNELS, SPACING_MHZ * 1000);
  ESP_LOGCONFIG(TAG, "  Dwell per channel: %" PRIu32 " us, report every %" PRIu32 " ms", this->dwell_us_, this->report_interval_ms_);
  ESP_LOGCONFIG(TAG, "  Scale (peak): ' ' < -93 dBm ... '@' >= -37 dBm, about 7 dB per step: \" .:-=+*#%%@\"");
  if (this->is_failed())
    ESP_LOGE(TAG, "  Communication with CC2500 failed!");
}

}  // namespace cc2500_scanner
}  // namespace esphome
