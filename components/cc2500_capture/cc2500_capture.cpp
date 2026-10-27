#include "cc2500_capture.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include <cinttypes>
#include <cmath>
#include <cstdio>

namespace esphome {
namespace cc2500_capture {

static const char *const TAG = "cc2500_capture";

static const uint8_t READ_SINGLE = 0x80;
static const uint8_t BURST = 0x40;
static const uint8_t SRES = 0x30;
static const uint8_t SRX = 0x34;
static const uint8_t SIDLE = 0x36;
static const uint8_t SFRX = 0x3A;
static const uint8_t PARTNUM = 0x30;
static const uint8_t VERSION = 0x31;
static const uint8_t RXBYTES = 0x3B;
static const uint8_t RXFIFO = 0x3F;

// Every capture: 24 payload bytes following the preamble + 2 appended status bytes.
static const uint8_t CAPTURE_LEN = 24;
static const uint8_t FRAME_LEN = CAPTURE_LEN + 2;
static const int RSSI_OFFSET = 72;
static const uint8_t MAX_LOGS_PER_SECOND = 8;

struct Profile {
  const char *name;
  uint8_t mdmcfg4;  // RX filter bandwidth + DRATE_E
  uint8_t mdmcfg3;  // DRATE_M
  uint8_t mod;      // MDMCFG2 MOD_FORMAT bits (6:4)
  uint8_t deviatn;
  uint8_t fsctrl1;  // IF frequency
  uint8_t foccfg;
  uint8_t bscfg;
};

// 26 MHz crystal. Values checked against the CC2500 datasheet formulas.
static const Profile PROFILES[] = {
    {"250 kBaud MSK, filter 812 kHz", 0x0D, 0x3B, 0x70, 0x00, 0x0C, 0x1D, 0x1C},
    {"250 kBaud 2-FSK (dev 83 kHz)", 0x0D, 0x3B, 0x00, 0x55, 0x0C, 0x1D, 0x1C},
    {"250 kBaud GFSK (dev 83 kHz)", 0x0D, 0x3B, 0x10, 0x55, 0x0C, 0x1D, 0x1C},
    {"500 kBaud MSK, filter 812 kHz", 0x0E, 0x3B, 0x70, 0x00, 0x0C, 0x1D, 0x1C},
    {"100 kBaud 2-FSK (dev 48 kHz)", 0x2B, 0xF8, 0x00, 0x47, 0x0A, 0x1D, 0x1C},
    {"100 kBaud GFSK (dev 48 kHz)", 0x2B, 0xF8, 0x10, 0x47, 0x0A, 0x1D, 0x1C},
    {"38.4 kBaud 2-FSK (dev 21 kHz)", 0x4A, 0x83, 0x00, 0x35, 0x0A, 0x16, 0x6C},
    {"38.4 kBaud GFSK (dev 21 kHz)", 0x4A, 0x83, 0x10, 0x35, 0x0A, 0x16, 0x6C},
    {"10 kBaud 2-FSK (dev 19 kHz)", 0x88, 0x93, 0x00, 0x34, 0x08, 0x16, 0x6C},
    {"2.4 kBaud 2-FSK (dev 38 kHz)", 0x86, 0x83, 0x00, 0x44, 0x08, 0x16, 0x6C},
};
static const int NUM_PROFILES = sizeof(PROFILES) / sizeof(PROFILES[0]);

uint8_t CC2500Capture::strobe_(uint8_t cmd) {
  this->enable();
  const uint8_t status = this->transfer_byte(cmd);
  this->disable();
  return status;
}

void CC2500Capture::write_register_(uint8_t addr, uint8_t value) {
  this->enable();
  this->transfer_byte(addr);
  this->transfer_byte(value);
  this->disable();
}

uint8_t CC2500Capture::read_register_(uint8_t addr) {
  this->enable();
  this->transfer_byte(addr | READ_SINGLE);
  const uint8_t value = this->transfer_byte(0x00);
  this->disable();
  return value;
}

uint8_t CC2500Capture::read_status_(uint8_t addr) {
  this->enable();
  this->transfer_byte(addr | READ_SINGLE | BURST);
  const uint8_t value = this->transfer_byte(0x00);
  this->disable();
  return value;
}

void CC2500Capture::read_fifo_(uint8_t *buf, uint8_t len) {
  this->enable();
  this->transfer_byte(RXFIFO | READ_SINGLE | BURST);
  for (uint8_t i = 0; i < len; i++)
    buf[i] = this->transfer_byte(0x00);
  this->disable();
}

void CC2500Capture::restart_rx_() {
  this->strobe_(SIDLE);
  this->strobe_(SFRX);
  this->strobe_(SRX);
}

void CC2500Capture::apply_profile_(int index) {
  const Profile &p = PROFILES[index];
  const uint32_t freq = static_cast<uint32_t>(lroundf(this->frequency_hz_ / 26.0e6f * 65536.0f));

  this->strobe_(SIDLE);
  // Trigger on the preamble instead of a sync word: SYNC = 0xAAAA and
  // SYNC_MODE 3 (30 of 32 bits) matches 32 bits of 1010... with up to two bit
  // errors. Whatever the remote sends after its preamble - its real sync word
  // and payload - then lands in the FIFO unfiltered.
  const uint8_t regs[][2] = {
      {0x00, 0x29},                   // IOCFG2
      {0x02, 0x06},                   // IOCFG0
      {0x03, 0x07},                   // FIFOTHR
      {0x04, 0xAA},                   // SYNC1 = preamble pattern
      {0x05, 0xAA},                   // SYNC0
      {0x06, CAPTURE_LEN},            // PKTLEN (fixed length)
      {0x07, 0x04},                   // PKTCTRL1: append RSSI/LQI, no address check
      {0x08, 0x00},                   // PKTCTRL0: fixed length, no CRC, no whitening
      {0x09, 0x00},                   // ADDR
      {0x0A, 0x00},                   // CHANNR
      {0x0B, p.fsctrl1},              // FSCTRL1
      {0x0C, 0x00},                   // FSCTRL0
      {0x0D, static_cast<uint8_t>((freq >> 16) & 0xFF)},  // FREQ2
      {0x0E, static_cast<uint8_t>((freq >> 8) & 0xFF)},   // FREQ1
      {0x0F, static_cast<uint8_t>(freq & 0xFF)},          // FREQ0
      {0x10, p.mdmcfg4},              // MDMCFG4
      {0x11, p.mdmcfg3},              // MDMCFG3
      {0x12, static_cast<uint8_t>(p.mod | 0x03)},         // MDMCFG2: modulation, 30/32 sync
      {0x13, 0x22},                   // MDMCFG1: FEC off
      {0x14, 0xF8},                   // MDMCFG0
      {0x15, p.deviatn},              // DEVIATN
      {0x16, 0x07},                   // MCSM2
      {0x17, 0x3C},                   // MCSM1: stay in RX after a packet
      {0x18, 0x18},                   // MCSM0: calibrate on IDLE->RX
      {0x19, p.foccfg},               // FOCCFG
      {0x1A, p.bscfg},                // BSCFG
      {0x1B, 0xC7},                   // AGCCTRL2
      {0x1C, 0x00},                   // AGCCTRL1
      {0x1D, 0xB2},                   // AGCCTRL0
      {0x21, 0xB6},                   // FREND1
      {0x22, 0x10},                   // FREND0
      {0x23, 0xEA},                   // FSCAL3
      {0x24, 0x0A},                   // FSCAL2
      {0x25, 0x00},                   // FSCAL1
      {0x26, 0x11},                   // FSCAL0
      {0x29, 0x59},                   // FSTEST
      {0x2C, 0x88},                   // TEST2
      {0x2D, 0x31},                   // TEST1
      {0x2E, 0x0B},                   // TEST0
  };
  for (const auto &r : regs)
    this->write_register_(r[0], r[1]);
  for (const auto &r : regs) {
    const uint8_t got = this->read_register_(r[0]);
    if (got != r[1])
      ESP_LOGW(TAG, "Register 0x%02X: wrote 0x%02X, read 0x%02X", r[0], r[1], got);
  }

  this->profile_ = index;
  this->profile_start_ms_ = millis();
  this->captures_ = 0;
  this->suppressed_ = 0;
  this->restart_rx_();
  ESP_LOGI(TAG, ">>> Profile %d/%d: %s @ %.3f MHz - press buttons on the remote now", index, NUM_PROFILES - 1,
           p.name, this->frequency_hz_ / 1e6f);
}

void CC2500Capture::setup() {
  ESP_LOGCONFIG(TAG, "Setting up CC2500 raw capture...");
  this->spi_setup();

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
  this->apply_profile_(this->fixed_profile_ >= 0 ? this->fixed_profile_ : 0);
}

void CC2500Capture::loop() {
  if (this->is_failed())
    return;

  const uint32_t now = millis();
  if (this->fixed_profile_ < 0 && now - this->profile_start_ms_ >= this->profile_time_ms_) {
    ESP_LOGI(TAG, "<<< Profile %d finished: %" PRIu32 " captures (%" PRIu32 " not logged)", this->profile_,
             this->captures_, this->suppressed_);
    this->apply_profile_((this->profile_ + 1) % NUM_PROFILES);
    return;
  }

  // RXBYTES can be unstable during active RX (erratum): read until two agree.
  const uint8_t b1 = this->read_status_(RXBYTES);
  const uint8_t b2 = this->read_status_(RXBYTES);
  if (b1 != b2)
    return;
  if (b2 & 0x80) {  // overflow
    this->restart_rx_();
    return;
  }

  uint8_t available = b2 & 0x7F;
  while (available >= FRAME_LEN) {
    uint8_t buf[FRAME_LEN];
    this->read_fifo_(buf, FRAME_LEN);
    available -= FRAME_LEN;
    this->captures_++;

    if (now - this->log_window_ms_ >= 1000) {
      this->log_window_ms_ = now;
      this->logs_in_window_ = 0;
    }
    if (this->logs_in_window_ >= MAX_LOGS_PER_SECOND) {
      this->suppressed_++;
      continue;
    }
    this->logs_in_window_++;

    char hex[CAPTURE_LEN * 3 + 1];
    for (uint8_t i = 0; i < CAPTURE_LEN; i++)
      snprintf(hex + i * 3, 4, "%02X ", buf[i]);
    hex[CAPTURE_LEN * 3 - 1] = '\0';
    const int rssi = static_cast<int8_t>(buf[CAPTURE_LEN]) / 2 - RSSI_OFFSET;
    ESP_LOGI(TAG, "[P%d] %4d dBm | %s", this->profile_, rssi, hex);
  }
}

void CC2500Capture::dump_config() {
  ESP_LOGCONFIG(TAG, "CC2500 raw capture:");
  ESP_LOGCONFIG(TAG, "  CC2500 PARTNUM=0x%02X VERSION=0x%02X", this->partnum_, this->version_);
  LOG_PIN("  CS Pin: ", this->cs_);
  ESP_LOGCONFIG(TAG, "  Frequency: %.3f MHz, trigger: preamble 0xAAAAAAAA (30/32 bits)", this->frequency_hz_ / 1e6f);
  if (this->fixed_profile_ >= 0) {
    ESP_LOGCONFIG(TAG, "  Fixed profile %d: %s", this->fixed_profile_, PROFILES[this->fixed_profile_].name);
  } else {
    ESP_LOGCONFIG(TAG, "  Cycling through %d profiles, %" PRIu32 " ms each", NUM_PROFILES, this->profile_time_ms_);
  }
  if (this->is_failed())
    ESP_LOGE(TAG, "  Communication with CC2500 failed!");
}

}  // namespace cc2500_capture
}  // namespace esphome
