# CLAUDE.md — notes for agents

Project: ESPHome external components for the older **L&S Emotion "Bielefeld"** furniture lighting
(e.g. in Hülsta cabinets): control unit "Emotion 4-Kanal Steuerung Bielefeld 12/24 V", 8-button remote
PCB `TxD8 LUS_RC24`. An ESP32 with a **CC2500** module acts as a second copy of the user's remote:
it sends the same frames and listens to the real remote. Integration targets: Home Assistant (native
API) and ioBroker (MQTT).

This is **not** the Frankfurt series. That one is covered by
<https://github.com/SergeyZh/emotion-control-ha> (2436.2 MHz, MSK, 250 kBaud, different frame,
absolute commands). Nothing of its protocol applies here — do not copy register values or command
codes across.

This file is **English only** — it is not translated.

## Documentation is bilingual
English (primary) and German. Naming: `NAME.md` English, `NAME.de.md` German. Currently: `README`,
`docs/PROTOCOL`, `docs/TOOLS`.

- **English is the source of truth.** Change it first, then port the same change to `.de.md`. A change
  in only one language is incomplete.
- Every doc starts with the language badge header linking both variants:
  ```markdown
  [![en](https://img.shields.io/badge/lang-en-blue.svg)](README.md)
  [![de](https://img.shields.io/badge/lang-de-yellow.svg)](README.de.md)
  ```
- Cross-links stay inside their language (`README.de.md` → `docs/PROTOCOL.de.md`). Anchors follow the
  translated headings (`#hardware-of-the-remote` vs `#hardware-der-fernbedienung`) — check them.
- German uses **"du"** (informal) and decimal commas (2,4 GHz). Terminology: remote / Fernbedienung,
  control unit / Steuerung, lamp / Lampe, channel / Kanal, frame / Paket, hold / halten.
- Never translated: log lines, code, YAML keys, register names, entity IDs and the **key names**
  (`sonne`, `schneeflocke`, `kanal_1` … `kanal_4`, `ein`, `aus` — German on purpose, as printed on the
  remote).
- Photos live in `docs/images/` (≤1400 px, no EXIF/GPS, serial-number labels blurred).

## Privacy — must never reach the repository
- **The user's real remote ID.** Examples and docs always use the placeholder
  `12 34 56 78 9A BC DE F0`. Captured frames shown in docs are recomputed for that ID (CRC included —
  `tools/decode_capture.py` verifies them). `grep` for the real ID before every commit.
- `remote_id` has **no default** and must not get one: a shipped ID would make other people's ESPs
  transmit as someone else's remote. Without it the hub only listens and logs the IDs it hears
  (that is how users find theirs).
- `secrets.yaml`, Wi-Fi/MQTT credentials, personal names, e-mail addresses, IP addresses of the
  author's network.

## Layout
- `components/ls_bielefeld/` — the main component (hub + platforms):
  - `__init__.py` — hub schema: SPI device, `cs_pin`, optional `remote_id`, `frequency`,
    `deviation`, `repeats`, `dim_time`, `color_time`. `AUTO_LOAD` button/light/number/text_sensor.
  - `ls_bielefeld.h/.cpp` — radio driver, TX queue, RX + dedup, state models, all platform classes.
  - `button.py` (`key` = one key or a list, `hold`), `light.py` (one dimmable light per `channel`),
    `number.py` (colour temperature 0–100 %), `text_sensor.py` (key pressed on the real remote).
- `components/cc2500_scanner/` — 2.4 GHz RSSI sweep with peak hold and narrow-peak detection.
- `components/cc2500_capture/` — raw capture triggered on the preamble, cycles 10 modem profiles.
- `tools/decode_capture.py` — realigns capture logs on `D3 91 D3 91`, checks the CRC, decodes fields.
- `examples/` — `find-remote-id.yaml`, `bielefeld.yaml` (native API), `bielefeld-mqtt.yaml`,
  `scanner.yaml`, `capture.yaml`.
- Only one CC2500 component may be used per config — they all drive the same chip.

## Hardware
- ESP32 DevKit (ESP-WROOM-32), framework esp-idf.
- CC2500 module with **26 MHz** crystal, powered from **3.3 V only**.
- Wiring: SCLK → GPIO18, SI/MOSI → GPIO23, SO/MISO → GPIO19, CSN → GPIO5. GDO0/GDO2 unused.
  (GPIO5 is a strapping pin; the ESPHome warning about it is harmless here.)
- Remote: STM8L101F3U6 (marking `L1S3`) + CC2500, CR2032. Its firmware was **not** read (SWIM did not
  respond); everything was found over the air. The user does not want further hardware work on the
  remote.

## Radio and frame (see `docs/PROTOCOL.md` for the full write-up)
- 2433.0 MHz (`FREQ 5D 93 B1`, `CHANNR 0`), **2-FSK ~10 kBaud** (`MDMCFG4 0x88`, `MDMCFG3 0x93`),
  RX filter 203 kHz, TX deviation 38 kHz (`DEVIATN 0x44`, accepted by the control unit, not measured).
- Sync `D3 91` with `SYNC_MODE` 30/32 → **sent twice** (`D3 91 D3 91` on air).
- Variable length, length byte `0x0E`, hardware **CRC-16** (poly 0x8005, init 0xFFFF) over length +
  payload. No FEC, no whitening. `PKTLEN 0x0E` caps garbage lengths after false syncs.
- Payload: `[09][key][cnt_hi][cnt_lo][00][seq][ID × 8]`.
  - Key bits: `01` sun, `02` snowflake, `04/08/10/20` channel 1–4, `40` on, `80` off. Keys held
    together OR their bits.
  - `seq` 0 = pressed; while held +1 about every **500 ms**; release = `key 00`, `seq` last+1.
  - Every frame is repeated (4–8× by the remote, `repeats` default 6 by us), ~20 ms on air each.
  - The 16-bit **counter** +1 per press. The control unit does **not** reject lower counters (verified:
    the remote kept working after the ESP sent with a counter far ahead). We still track the highest
    counter seen and store it in flash.

## CC2500 rakes
- **Status registers (0x30–0x3D) must be read with READ|BURST (0xC0).** A plain read of 0x35 is the
  `STX` strobe and starts transmitting; 0x30 would be `SRES`.
- `RXBYTES` erratum: read it twice until both reads agree.
- After `SRX` poll `MARCSTATE` until RX (0x0D) instead of assuming it (seen: stuck at 0x08 for
  ~200 µs). The same fix was proposed to Sergey's driver.
- `poll_rx_` drains **all** complete 17-byte frames (length + 14 + RSSI/LQI); a misaligned length or a
  bad CRC flushes and restarts RX. A partial frame older than 100 ms also restarts RX.
- `MCSM1 0x0F`: back to RX after TX, so no explicit restart is needed after each frame; the loop only
  waits until `MARCSTATE` leaves the TX/settling states (timeout 100 ms).

## Behaviour of the control unit (measured from videos + ESP logs)
- Channel key short = toggle that lamp. Ein / Aus = **all** lamps; brightness is remembered.
- Channel key **held**: changes only from the 2nd frame on (**~500 ms delay**, `HOLD_DELAY_MS`), full
  range ~**1 s** (`dim_time`), **direction alternates with every hold**. At an end it **dwells** (≈3 s at
  max, ≈1 s at min) and then reverses while still held. A minimum brightness remains.
- In both tests the first hold after Aus → Ein went **down** (lamp was at max).
- Sun / snowflake held: always warmer / colder, **stops at the end, no reversal**, full range ~1.2 s
  (`color_time`). Colour applies to all lamps. Effect of a *short* sun/snowflake press is unknown
  (modelled as no change).
- The control gives **no feedback** — every state in the component is an estimate.

## State model (ls_bielefeld.cpp)
- `ChannelModel {on, level 0..1, next_up}` per channel, `color_` 0 (cold) … 1 (warm).
- One code path updates the model: `apply_key_effect_(key, hold_ms)`, called for our own presses at
  queue time and for the remote's presses on release (hold time = release − first frame; timeout
  1.5 s if the release is lost). `simulate_dim` models delay, ramp, dwell and reversal.
- Setting a brightness (`apply_channel_target_`): switch on if needed; if `next_up` points the wrong
  way, first run into that end (+`END_MARGIN_MS` 300 ms, safely inside the dwell); then hold for
  `500 ms + |Δ| · dim_time`. Targets within `END_THRESHOLD` (2 %) of an end run into the end and so
  re-sync the model. Colour works the same without the direction problem.
- Requests are **debounced 300 ms** (sliders) and planned against the *predicted* model, so queued
  sequences and new requests compose.
- Channel requests are handled **together** once none arrived for 300 ms (`apply_channel_targets_`):
  all needed toggles go out as **one short press with the OR'd channel bits**, and channels with the
  same level, direction and hold plan are dimmed in one combined hold. Reason: an Alexa group sends
  three separate requests, and three separate channel presses 250 ms apart were not all executed by
  the control unit (only one lamp switched). Whether a combined hold keeps a separate direction per
  channel is unverified, so lamps in different states are still dimmed one after another.
- Presses are queued back to back with `press_gap` (option, default 250 ms) via `tx_free_at_ms_` — before that fix,
  a second press queued during a hold went out immediately and overlapped on air.
- Feedback to the entities: `publish_models_()` → `LSChannelLight::set_estimate()` / number
  `publish_state`. **No suppress flag:** `make_call().perform()` calls `write_state` lazily, so instead
  `request_channel()` ignores targets that equal the model. Keep it that way.
- Boot: the first `write_state` before 15 s uptime is the restored state — adopt it as the model, do
  not transmit (the lamp keeps its own state; retransmitting could undo a remote press made while the
  ESP was down).
- Light schema overrides `default_transition_length: 0s` and `gamma_correct: 1.0`; use
  `state->remote_values` (target), never interpolated `current_values`.

## Integration notes
- MQTT: `topic_prefix` (user: `huelsta`), `discovery: false` for ioBroker, `log_topic: null`,
  `api: reboot_timeout: 0s` when no API client is connected. Light command JSON
  `{"state":"ON","brightness":0-255}`, button `PRESS`, number value as string.
- ioBroker's MQTT adapter in client mode needs a broker reachable from the LAN (Mosquitto 2.x listens
  on localhost only unless `listener 1883` is configured). In the adapter, disable "publish own states
  on connect", otherwise old commands are replayed on reconnect.

## Workflow (dev)
- ESPHome cannot be installed in the cloud sandbox (PyPI blocked). C++ is syntax-checked with
  `g++ -std=c++17 -fsyntax-only` against small mock headers of the ESPHome API, and the dimming logic
  is exercised by a host test that links `ls_bielefeld.cpp` with a controllable `millis()`. Real
  compilation happens on the user's ESPHome (ioBroker ESPHome adapter dashboard, 2026.6.x).
- Check ESPHome API details against the ESPHome source (GitHub), not from memory.
- YAML examples: validate with a YAML loader that accepts `!secret`.
- Commits: no `Co-Authored-By` and no session lines in commit messages or PR descriptions (the user
  asked for this).

### Signs of a healthy start in the log
```
L&S Bielefeld control:
  CC2500 PARTNUM=0x80 VERSION=0x03      (VERSION 0x00/0xFF = wiring problem)
  Remote ID: 12 34 56 78 9A BC DE F0
  Frequency: 2433.000 MHz, 2-FSK ~10 kBaud, deviation register 0x44 (38.0 kHz requested)
```
and every key on the real remote shows up as `Remote: <key> (counter …, -xx dBm)`.
