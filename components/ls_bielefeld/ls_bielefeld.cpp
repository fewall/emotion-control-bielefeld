#include "ls_bielefeld.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#include <cinttypes>
#include <cmath>
#include <cstdio>

namespace esphome {
namespace ls_bielefeld {

static const char *const TAG = "ls_bielefeld";

static const uint8_t READ_SINGLE = 0x80;
static const uint8_t BURST = 0x40;
static const uint8_t SRES = 0x30;
static const uint8_t SRX = 0x34;
static const uint8_t STX = 0x35;
static const uint8_t SIDLE = 0x36;
static const uint8_t SFRX = 0x3A;
static const uint8_t SFTX = 0x3B;
static const uint8_t PARTNUM = 0x30;
static const uint8_t VERSION = 0x31;
static const uint8_t MARCSTATE = 0x35;
static const uint8_t RXBYTES = 0x3B;
static const uint8_t PATABLE = 0x3E;
static const uint8_t FIFO = 0x3F;

static const uint8_t MARC_IDLE = 0x01;
static const uint8_t MARC_RX = 0x0D;
static const uint8_t MARC_TX = 0x13;
static const uint8_t MARC_TX_END = 0x14;
static const uint8_t MARC_TXFIFO_UNDERFLOW = 0x16;

static const uint8_t PAYLOAD_LEN = 14;
static const uint8_t RX_FRAME_LEN = 1 + PAYLOAD_LEN + 2;  // length byte + payload + RSSI/LQI
static const uint8_t TYPE_BYTE = 0x09;
static const int RSSI_OFFSET = 72;

// Timing modelled on the captures: while a key is held the remote sends a new
// sequence number roughly every 0.5 s; on release it sends key=0 once more.
static const uint32_t HOLD_STEP_MS = 500;
static const uint32_t SHORT_RELEASE_DELAY_MS = 150;
static const uint8_t STEP_COPIES = 3;
// Pause between two emulated presses, so the control sees them as separate presses.
static const uint32_t PRESS_GAP_MS = 250;

// Behaviour of the control while a key is held, measured from videos:
// it starts changing only with the second frame (~0.5 s after pressing), and when a
// held channel key reaches an end it stays there for a while before reversing.
static const uint32_t HOLD_DELAY_MS = 500;
static const float DWELL_MAX_MS = 3000.0f;
static const float DWELL_MIN_MS = 1000.0f;
// Extra hold time when an end is the goal - lands safely inside the dwell at that end.
static const uint32_t END_MARGIN_MS = 300;
static const float LEVEL_TOLERANCE = 0.03f;
static const float MIN_PUBLISHED_BRIGHTNESS = 0.01f;
// Requested values this close to an end (e.g. 1 % = 3/255) mean "run into that end".
static const float END_THRESHOLD = 0.02f;
static const uint32_t DEBOUNCE_MS = 300;

static float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

// Dimming of one channel while its key is held for active_ms (after the initial delay).
static void simulate_dim(ChannelModel &m, float active_ms, float ramp_ms) {
  bool up = m.next_up;
  float t = active_ms;
  for (int guard = 0; guard < 20; guard++) {
    const float to_end = (up ? 1.0f - m.level : m.level) * ramp_ms;
    if (t <= to_end) {
      m.level += (up ? t : -t) / ramp_ms;
      break;
    }
    t -= to_end;
    m.level = up ? 1.0f : 0.0f;
    const float dwell = up ? DWELL_MAX_MS : DWELL_MIN_MS;
    if (t <= dwell)
      break;
    t -= dwell;
    up = !up;
  }
  m.level = clamp01(m.level);
  m.next_up = !up;
}

std::string LSBielefeld::key_name(uint8_t key) {
  static const char *const NAMES[8] = {"Sonne", "Schneeflocke", "Kanal 1", "Kanal 2",
                                       "Kanal 3", "Kanal 4",      "Ein",     "Aus"};
  if (key == 0)
    return "losgelassen";
  std::string out;
  for (int bit = 0; bit < 8; bit++) {
    if (key & (1 << bit)) {
      if (!out.empty())
        out += " + ";
      out += NAMES[bit];
    }
  }
  return out;
}

uint8_t LSBielefeld::strobe_(uint8_t cmd) {
  this->enable();
  const uint8_t status = this->transfer_byte(cmd);
  this->disable();
  return status;
}

void LSBielefeld::write_register_(uint8_t addr, uint8_t value) {
  this->enable();
  this->transfer_byte(addr);
  this->transfer_byte(value);
  this->disable();
}

uint8_t LSBielefeld::read_register_(uint8_t addr) {
  this->enable();
  this->transfer_byte(addr | READ_SINGLE);
  const uint8_t value = this->transfer_byte(0x00);
  this->disable();
  return value;
}

uint8_t LSBielefeld::read_status_(uint8_t addr) {
  this->enable();
  this->transfer_byte(addr | READ_SINGLE | BURST);
  const uint8_t value = this->transfer_byte(0x00);
  this->disable();
  return value;
}

void LSBielefeld::write_burst_(uint8_t addr, const uint8_t *data, uint8_t len) {
  this->enable();
  this->transfer_byte(addr | BURST);
  for (uint8_t i = 0; i < len; i++)
    this->transfer_byte(data[i]);
  this->disable();
}

void LSBielefeld::read_fifo_(uint8_t *buf, uint8_t len) {
  this->enable();
  this->transfer_byte(FIFO | READ_SINGLE | BURST);
  for (uint8_t i = 0; i < len; i++)
    buf[i] = this->transfer_byte(0x00);
  this->disable();
}

bool LSBielefeld::wait_state_(uint8_t state, uint32_t timeout_us) {
  const uint32_t start = micros();
  while ((this->read_status_(MARCSTATE) & 0x1F) != state) {
    if (micros() - start > timeout_us)
      return false;
  }
  return true;
}

static uint8_t deviation_register(float hz) {
  // f_dev = f_xosc / 2^17 * (8 + M) * 2^E
  uint8_t best = 0x44;
  float best_err = 1e12f;
  for (int e = 0; e < 8; e++) {
    for (int m = 0; m < 8; m++) {
      const float f = 26.0e6f / 131072.0f * (8 + m) * static_cast<float>(1 << e);
      const float err = std::fabs(f - hz);
      if (err < best_err) {
        best_err = err;
        best = static_cast<uint8_t>((e << 4) | m);
      }
    }
  }
  return best;
}

bool LSBielefeld::configure_radio_() {
  const uint32_t freq = static_cast<uint32_t>(lroundf(this->frequency_hz_ / 26.0e6f * 65536.0f));
  const uint8_t regs[][2] = {
      {0x00, 0x29},  // IOCFG2
      {0x02, 0x06},  // IOCFG0  sync sent/received .. end of packet
      {0x03, 0x07},  // FIFOTHR
      {0x04, 0xD3},  // SYNC1
      {0x05, 0x91},  // SYNC0
      {0x06, PAYLOAD_LEN},  // PKTLEN: maximum accepted length
      {0x07, 0x0C},  // PKTCTRL1: CRC autoflush, append RSSI/LQI
      {0x08, 0x05},  // PKTCTRL0: variable length, hardware CRC, no whitening
      {0x09, 0x00},  // ADDR
      {0x0A, 0x00},  // CHANNR
      {0x0B, 0x08},  // FSCTRL1
      {0x0C, 0x00},  // FSCTRL0
      {0x0D, static_cast<uint8_t>((freq >> 16) & 0xFF)},  // FREQ2
      {0x0E, static_cast<uint8_t>((freq >> 8) & 0xFF)},   // FREQ1
      {0x0F, static_cast<uint8_t>(freq & 0xFF)},          // FREQ0
      {0x10, 0x88},  // MDMCFG4: RX filter 203 kHz, DRATE_E=8
      {0x11, 0x93},  // MDMCFG3: ~10 kBaud
      {0x12, 0x03},  // MDMCFG2: 2-FSK, 30/32 sync bits (sync sent twice)
      {0x13, 0x22},  // MDMCFG1: 4 preamble bytes, FEC off
      {0x14, 0xF8},  // MDMCFG0
      {0x15, deviation_register(this->deviation_hz_)},  // DEVIATN
      {0x16, 0x07},  // MCSM2
      {0x17, 0x0F},  // MCSM1: CCA off, stay in RX after RX, back to RX after TX
      {0x18, 0x18},  // MCSM0: calibrate on IDLE->RX/TX
      {0x19, 0x16},  // FOCCFG
      {0x1A, 0x6C},  // BSCFG
      {0x1B, 0xC7},  // AGCCTRL2
      {0x1C, 0x00},  // AGCCTRL1
      {0x1D, 0xB2},  // AGCCTRL0
      {0x21, 0xB6},  // FREND1
      {0x22, 0x10},  // FREND0: PATABLE[0]
      {0x23, 0xEA},  // FSCAL3
      {0x24, 0x0A},  // FSCAL2
      {0x25, 0x00},  // FSCAL1
      {0x26, 0x11},  // FSCAL0
      {0x29, 0x59},  // FSTEST
      {0x2C, 0x88},  // TEST2
      {0x2D, 0x31},  // TEST1
      {0x2E, 0x0B},  // TEST0
  };
  for (const auto &r : regs)
    this->write_register_(r[0], r[1]);
  bool ok = true;
  for (const auto &r : regs) {
    const uint8_t got = this->read_register_(r[0]);
    if (got != r[1]) {
      ESP_LOGE(TAG, "Register 0x%02X: wrote 0x%02X, read 0x%02X", r[0], r[1], got);
      ok = false;
    }
  }
  const uint8_t pa[1] = {0xFF};  // maximum output power (~+1 dBm)
  this->write_burst_(PATABLE, pa, 1);
  return ok;
}

void LSBielefeld::restart_rx_() {
  this->strobe_(SIDLE);
  this->wait_state_(MARC_IDLE, 1000);
  this->strobe_(SFRX);
  this->strobe_(SRX);
  this->rx_wait_since_ms_ = 0;
}

void LSBielefeld::setup() {
  ESP_LOGCONFIG(TAG, "Setting up L&S Bielefeld control...");
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
  if (!this->configure_radio_()) {
    this->mark_failed();
    return;
  }

  this->pref_ = global_preferences->make_preference<uint16_t>(fnv1_hash("ls_bielefeld_counter"));
  uint16_t stored = 0;
  if (this->pref_.load(&stored)) {
    this->counter_ = stored;
    this->counter_known_ = true;
  }
  this->color_pref_ = global_preferences->make_preference<float>(fnv1_hash("ls_bielefeld_color"));
  float color = 1.0f;
  if (this->color_pref_.load(&color) && color >= 0.0f && color <= 1.0f)
    this->color_ = color;
  if (this->color_number_ != nullptr)
    this->color_number_->publish_state(roundf(this->color_ * 100.0f));

  this->restart_rx_();
  this->publish_key_("");
}

void LSBielefeld::update_counter_(uint16_t seen) {
  // Keep our counter at or ahead of the real remote, with 16-bit wrap-around.
  if (!this->counter_known_ || static_cast<int16_t>(seen - this->counter_) > 0) {
    this->counter_ = seen;
    this->counter_known_ = true;
    this->pref_.save(&this->counter_);
  }
}

void LSBielefeld::publish_key_(const char *text) {
  if (this->key_sensor_ != nullptr)
    this->key_sensor_->publish_state(text);
}

static const uint8_t KEY_EIN = 0x40;
static const uint8_t KEY_AUS = 0x80;
static const uint8_t CHANNEL_KEYS[4] = {0x04, 0x08, 0x10, 0x20};
static const uint32_t PRESS_TIMEOUT_MS = 1500;

static const uint8_t KEY_SONNE = 0x01;
static const uint8_t KEY_SCHNEE = 0x02;

void LSBielefeld::apply_key_effect_(uint8_t key, uint32_t hold_ms) {
  const bool held = hold_ms >= HOLD_DELAY_MS;
  const float active = held ? static_cast<float>(hold_ms - HOLD_DELAY_MS) : 0.0f;
  for (int i = 0; i < 4; i++) {
    ChannelModel &m = this->channels_[i];
    if (key & KEY_EIN) {
      m.on = true;  // brightness is remembered by the control
    } else if (key & KEY_AUS) {
      m.on = false;
    } else if (key & CHANNEL_KEYS[i]) {
      if (held) {
        m.on = true;  // dimming needs the lamp on
        simulate_dim(m, active, static_cast<float>(this->dim_time_ms_));
      } else {
        m.on = !m.on;  // short press toggles the channel
      }
    }
  }
  // Colour temperature: no change of direction, it just stops at either end.
  if (held && (key & (KEY_SONNE | KEY_SCHNEE)) && !(key & KEY_SONNE && key & KEY_SCHNEE)) {
    const float step = active / static_cast<float>(this->color_time_ms_);
    this->color_ = clamp01(this->color_ + ((key & KEY_SONNE) ? step : -step));
    this->color_pref_.save(&this->color_);
  }
  if (!this->batch_)
    this->publish_models_();
}

void LSBielefeld::publish_models_() {
  for (int i = 0; i < 4; i++)
    if (this->lights_[i] != nullptr)
      this->lights_[i]->set_estimate(this->channels_[i].on, this->channels_[i].level);
  if (this->color_number_ != nullptr) {
    const float value = roundf(this->color_ * 100.0f);
    if (!this->color_number_->has_state() || this->color_number_->state != value)
      this->color_number_->publish_state(value);
  }
}

void LSBielefeld::adopt_channel(uint8_t channel, bool on, float brightness) {
  if (channel < 1 || channel > 4)
    return;
  ChannelModel &m = this->channels_[channel - 1];
  m.on = on;
  m.level = brightness <= END_THRESHOLD ? 0.0f : clamp01(brightness);
  // Unknown after a reboot. At an end the direction is clear; in between, the
  // first hold after switching on was "down" in every test.
  m.next_up = m.level <= 0.0f;
}

void LSBielefeld::request_channel(uint8_t channel, bool on, float brightness) {
  if (channel < 1 || channel > 4)
    return;
  const int i = channel - 1;
  const ChannelModel &m = this->channels_[i];
  const float level = brightness <= END_THRESHOLD ? 0.0f : clamp01(brightness);
  PendingTarget &p = this->pending_[i];
  if (on == m.on && (!on || fabsf(level - m.level) < 0.005f)) {
    p.active = false;  // matches the model, e.g. our own published estimate
    return;
  }
  p.active = true;
  p.on = on;
  p.value = level;
  p.since_ms = millis();
}

void LSBielefeld::request_color(float warmth) {
  warmth = clamp01(warmth);
  if (fabsf(warmth - this->color_) < 0.005f) {
    this->color_pending_.active = false;
    this->publish_models_();
    return;
  }
  this->color_pending_.active = true;
  this->color_pending_.value = warmth;
  this->color_pending_.since_ms = millis();
}

void LSBielefeld::apply_channel_target_(int i, bool on, float target) {
  const uint8_t key = CHANNEL_KEYS[i];
  ChannelModel &m = this->channels_[i];
  const float ramp = static_cast<float>(this->dim_time_ms_);
  this->batch_ = true;
  if (!on) {
    if (m.on)
      this->press(key, 0);
  } else {
    if (!m.on)
      this->press(key, 0);  // switch on, the control restores the last brightness
    const bool to_end = target >= 1.0f - END_THRESHOLD || target <= END_THRESHOLD;
    if (to_end)
      target = target >= 1.0f - END_THRESHOLD ? 1.0f : 0.0f;
    const float delta = target - m.level;
    if (fabsf(delta) >= LEVEL_TOLERANCE || (to_end && delta != 0.0f)) {
      const bool want_up = delta > 0.0f;
      if (m.next_up != want_up) {
        // The next hold would go the wrong way: run it into its end first.
        // That also re-synchronises the model.
        const float dist = m.next_up ? 1.0f - m.level : m.level;
        this->press(key, HOLD_DELAY_MS + static_cast<uint32_t>(dist * ramp) + END_MARGIN_MS);
      }
      const float dist = fabsf(target - m.level);
      uint32_t hold = HOLD_DELAY_MS + static_cast<uint32_t>(dist * ramp);
      if (to_end)
        hold += END_MARGIN_MS;
      if (dist > 0.0f)
        this->press(key, hold);
    }
  }
  this->batch_ = false;
  ESP_LOGI(TAG, "Kanal %d -> %s, Helligkeit ~%.0f%%", i + 1, m.on ? "an" : "aus", m.level * 100.0f);
  this->publish_models_();
}

void LSBielefeld::apply_color_target_(float target) {
  const float ramp = static_cast<float>(this->color_time_ms_);
  const bool to_end = target >= 1.0f - END_THRESHOLD || target <= END_THRESHOLD;
  if (to_end)
    target = target >= 1.0f - END_THRESHOLD ? 1.0f : 0.0f;
  const float delta = target - this->color_;
  if (fabsf(delta) >= LEVEL_TOLERANCE || (to_end && delta != 0.0f)) {
    uint32_t hold = HOLD_DELAY_MS + static_cast<uint32_t>(fabsf(delta) * ramp);
    if (to_end)
      hold += END_MARGIN_MS;
    this->batch_ = true;
    this->press(delta > 0.0f ? KEY_SONNE : KEY_SCHNEE, hold);
    this->batch_ = false;
  }
  ESP_LOGI(TAG, "Farbtemperatur -> ~%.0f%% warm", this->color_ * 100.0f);
  this->publish_models_();
}

void LSBielefeld::finish_remote_press_() {
  if (!this->rx_press_open_)
    return;
  this->rx_press_open_ = false;
  uint32_t hold_ms = 0;
  if (this->rx_press_held_) {
    hold_ms = this->rx_last_frame_ms_ - this->rx_press_start_ms_;
    if (hold_ms < HOLD_DELAY_MS)
      hold_ms = HOLD_DELAY_MS;
  }
  this->apply_key_effect_(this->rx_pressed_key_, hold_ms);
}

void LSBielefeld::press(uint8_t key, uint32_t hold_ms, bool update_lights) {
  if (this->is_failed())
    return;
  if (!this->has_remote_id_) {
    ESP_LOGE(TAG, "Cannot send %s: no remote_id configured (listening only). "
                  "Press a key on your remote, copy the ID from the log into remote_id.",
             key_name(key).c_str());
    return;
  }
  if (this->queue_.size() > 120) {
    ESP_LOGW(TAG, "Transmit queue full, ignoring key %s", key_name(key).c_str());
    return;
  }
  if (!this->counter_known_)
    ESP_LOGW(TAG, "Counter not synced yet - press any key on the real remote once so the ESP can learn it.");

  this->counter_++;
  this->counter_known_ = true;
  this->pref_.save(&this->counter_);
  const uint16_t cnt = this->counter_;

  // Presses queued back to back must not overlap on air.
  uint32_t start = millis();
  if (static_cast<int32_t>(this->tx_free_at_ms_ - start) > 0)
    start = this->tx_free_at_ms_;
  uint8_t seq = 0;
  for (uint8_t i = 0; i < this->repeats_; i++)
    this->queue_.push_back({key, cnt, seq, start});
  const uint32_t steps = hold_ms / HOLD_STEP_MS;
  for (uint32_t s = 1; s <= steps && seq < 0xFE; s++) {
    seq++;
    for (uint8_t i = 0; i < STEP_COPIES; i++)
      this->queue_.push_back({key, cnt, seq, start + s * HOLD_STEP_MS});
  }
  seq++;
  const uint32_t release_at = start + (hold_ms > 0 ? hold_ms : SHORT_RELEASE_DELAY_MS);
  for (uint8_t i = 0; i < STEP_COPIES; i++)
    this->queue_.push_back({0x00, cnt, seq, release_at});
  this->tx_free_at_ms_ = release_at + PRESS_GAP_MS;

  if (hold_ms > 0) {
    ESP_LOGI(TAG, "Sending %s (gehalten %u ms, counter %u)", key_name(key).c_str(), (unsigned) hold_ms, cnt);
  } else {
    ESP_LOGI(TAG, "Sending %s (counter %u)", key_name(key).c_str(), cnt);
  }
  if (update_lights)
    this->apply_key_effect_(key, hold_ms);
}

void LSBielefeld::send_frame_(const TxFrame &f) {
  uint8_t pkt[1 + PAYLOAD_LEN];
  pkt[0] = PAYLOAD_LEN;
  pkt[1] = TYPE_BYTE;
  pkt[2] = f.key;
  pkt[3] = static_cast<uint8_t>(f.counter >> 8);
  pkt[4] = static_cast<uint8_t>(f.counter & 0xFF);
  pkt[5] = 0x00;
  pkt[6] = f.seq;
  for (int i = 0; i < 8; i++)
    pkt[7 + i] = this->remote_id_[i];

  this->strobe_(SIDLE);
  this->wait_state_(MARC_IDLE, 1000);
  this->strobe_(SFTX);
  this->write_burst_(FIFO, pkt, sizeof(pkt));
  this->strobe_(STX);  // calibrates (~0.8 ms), sends preamble+sync+packet+CRC, then returns to RX
  this->transmitting_ = true;
  this->tx_start_ms_ = millis();
}

void LSBielefeld::loop() {
  if (this->is_failed())
    return;
  const uint32_t now = millis();

  if (this->rx_press_open_ && now - this->rx_last_frame_ms_ > PRESS_TIMEOUT_MS) {
    this->rx_last_frame_ms_ += 250;  // release frame was not received: assume it came soon after
    this->finish_remote_press_();
  }

  // Requested targets, debounced so a slider being dragged does not queue many presses.
  for (int i = 0; i < 4; i++) {
    PendingTarget &p = this->pending_[i];
    if (p.active && now - p.since_ms >= DEBOUNCE_MS && this->queue_.size() < 60) {
      p.active = false;
      this->apply_channel_target_(i, p.on, p.value);
    }
  }
  if (this->color_pending_.active && now - this->color_pending_.since_ms >= DEBOUNCE_MS &&
      this->queue_.size() < 60) {
    this->color_pending_.active = false;
    this->apply_color_target_(this->color_pending_.value);
  }

  if (this->transmitting_) {
    // A 14-byte frame at 10 kBaud takes ~20 ms on air.
    if (now - this->tx_start_ms_ < 3)
      return;
    const uint8_t state = this->read_status_(MARCSTATE) & 0x1F;
    const bool busy = state == MARC_TX || state == MARC_TX_END || (state >= 0x03 && state <= 0x0C) ||
                      state == 0x12 || state == 0x15;  // calibration / settling / FSTXON / RXTX settling
    if (busy && now - this->tx_start_ms_ < 100)
      return;
    if (state == MARC_TXFIFO_UNDERFLOW)
      ESP_LOGW(TAG, "TX FIFO underflow");
    if (busy)
      ESP_LOGW(TAG, "Transmission timed out (MARCSTATE=0x%02X)", state);
    this->transmitting_ = false;
    if (state != MARC_RX)
      this->restart_rx_();
    return;
  }

  if (!this->queue_.empty() && static_cast<int32_t>(now - this->queue_.front().not_before_ms) >= 0) {
    const TxFrame f = this->queue_.front();
    this->queue_.erase(this->queue_.begin());
    this->send_frame_(f);
    return;
  }

  this->poll_rx_();
}

void LSBielefeld::poll_rx_() {
  const uint8_t b1 = this->read_status_(RXBYTES);
  const uint8_t b2 = this->read_status_(RXBYTES);
  if (b1 != b2)
    return;
  if (b2 & 0x80) {  // FIFO overflow
    this->restart_rx_();
    return;
  }
  uint8_t available = b2 & 0x7F;
  if (available == 0) {
    this->rx_wait_since_ms_ = 0;
    return;
  }
  if (available < RX_FRAME_LEN) {
    // A shorter (non-Bielefeld) packet that passed the CRC would otherwise sit in the FIFO forever.
    if (this->rx_wait_since_ms_ == 0) {
      this->rx_wait_since_ms_ = millis();
    } else if (millis() - this->rx_wait_since_ms_ > 100) {
      this->restart_rx_();
    }
    return;
  }

  while (available >= RX_FRAME_LEN) {
    uint8_t buf[RX_FRAME_LEN];
    this->read_fifo_(buf, RX_FRAME_LEN);
    available -= RX_FRAME_LEN;
    if (buf[0] != PAYLOAD_LEN || !(buf[RX_FRAME_LEN - 1] & 0x80)) {
      this->restart_rx_();  // misaligned or bad CRC: start clean
      return;
    }
    const int rssi = static_cast<int8_t>(buf[RX_FRAME_LEN - 2]) / 2 - RSSI_OFFSET;
    this->handle_frame_(buf + 1, rssi);
  }
  this->rx_wait_since_ms_ = 0;
}

void LSBielefeld::handle_frame_(const uint8_t *p, int rssi) {
  if (p[0] != TYPE_BYTE)
    return;
  bool ours = this->has_remote_id_;
  for (int i = 0; i < 8 && ours; i++)
    if (p[6 + i] != this->remote_id_[i])
      ours = false;
  const uint8_t key = p[1];
  const uint16_t cnt = (static_cast<uint16_t>(p[2]) << 8) | p[3];
  const uint8_t seq = p[5];

  if (!ours) {
    if (millis() - this->foreign_logged_ms_ > 1000) {
      this->foreign_logged_ms_ = millis();
      ESP_LOGI(TAG, "%s %02X %02X %02X %02X %02X %02X %02X %02X: %s (counter %u, %d dBm)",
               this->has_remote_id_ ? "Other remote" : "Remote found - remote_id:", p[6], p[7], p[8], p[9], p[10],
               p[11], p[12], p[13], key_name(key).c_str(), cnt, rssi);
    }
    return;
  }

  // Every frame is repeated several times on air - act on the first copy only.
  if (this->rx_have_last_ && key == this->rx_last_key_ && cnt == this->rx_last_counter_ && seq == this->rx_last_seq_)
    return;
  this->rx_have_last_ = true;
  this->rx_last_key_ = key;
  this->rx_last_counter_ = cnt;
  this->rx_last_seq_ = seq;

  this->update_counter_(cnt);
  this->rx_last_frame_ms_ = millis();

  if (key != 0 && seq == 0) {
    this->finish_remote_press_();  // a previous press whose release we missed
    this->rx_pressed_key_ = key;
    this->rx_press_open_ = true;
    this->rx_press_held_ = false;
    this->rx_press_start_ms_ = this->rx_last_frame_ms_;
    const std::string name = key_name(key);
    ESP_LOGI(TAG, "Remote: %s (counter %u, %d dBm)", name.c_str(), cnt, rssi);
    this->publish_key_(name.c_str());
  } else if (key != 0 && seq >= 1) {
    this->rx_pressed_key_ = key;
    this->rx_press_open_ = true;
    if (this->rx_press_held_)
      return;  // already reported as held
    this->rx_press_held_ = true;
    const std::string text = key_name(key) + " (gehalten)";
    ESP_LOGI(TAG, "Remote: %s", text.c_str());
    this->publish_key_(text.c_str());
  } else if (key == 0) {
    ESP_LOGD(TAG, "Remote: %s released after %u steps", key_name(this->rx_pressed_key_).c_str(), seq);
    this->finish_remote_press_();
    this->rx_pressed_key_ = 0;
    this->publish_key_("");
  }
}

void LSBielefeld::dump_config() {
  ESP_LOGCONFIG(TAG, "L&S Bielefeld control:");
  ESP_LOGCONFIG(TAG, "  CC2500 PARTNUM=0x%02X VERSION=0x%02X", this->partnum_, this->version_);
  LOG_PIN("  CS Pin: ", this->cs_);
  if (this->has_remote_id_) {
    ESP_LOGCONFIG(TAG, "  Remote ID: %02X %02X %02X %02X %02X %02X %02X %02X", this->remote_id_[0],
                  this->remote_id_[1], this->remote_id_[2], this->remote_id_[3], this->remote_id_[4],
                  this->remote_id_[5], this->remote_id_[6], this->remote_id_[7]);
  } else {
    ESP_LOGCONFIG(TAG, "  Remote ID: not set - listening only. Press a key on your remote to see its ID.");
  }
  ESP_LOGCONFIG(TAG, "  Frequency: %.3f MHz, 2-FSK ~10 kBaud, deviation register 0x%02X (%.1f kHz requested)",
                this->frequency_hz_ / 1e6f, deviation_register(this->deviation_hz_), this->deviation_hz_ / 1e3f);
  if (this->counter_known_) {
    ESP_LOGCONFIG(TAG, "  Counter: %u", this->counter_);
  } else {
    ESP_LOGCONFIG(TAG, "  Counter: unknown - press a key on the real remote once");
  }
  if (this->is_failed())
    ESP_LOGE(TAG, "  Communication with CC2500 failed!");
}

void LSChannelLight::write_state(light::LightState *state) {
  if (this->parent_ == nullptr)
    return;
  // Use the requested target, not the values of a running transition.
  const auto &v = state->remote_values;
  const bool on = v.is_on();
  const float brightness = v.get_brightness();
  if (!this->initialized_) {
    this->initialized_ = true;
    if (millis() < 15000) {
      // First write right after boot restores the last known state: adopt it, do not transmit.
      this->parent_->adopt_channel(this->channel_, on, brightness);
      return;
    }
  }
  this->parent_->request_channel(this->channel_, on, brightness);
}

void LSChannelLight::set_estimate(bool on, float level) {
  this->initialized_ = true;
  if (this->state_ == nullptr)
    return;
  const float brightness = level < MIN_PUBLISHED_BRIGHTNESS ? MIN_PUBLISHED_BRIGHTNESS : level;
  const auto &v = this->state_->remote_values;
  if (v.is_on() == on && (!on || fabsf(v.get_brightness() - brightness) < 0.005f))
    return;
  auto call = this->state_->make_call();
  call.set_state(on);
  if (on)
    call.set_brightness(brightness);
  call.set_transition_length(0);
  call.perform();
}

}  // namespace ls_bielefeld
}  // namespace esphome
