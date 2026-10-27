[![en](https://img.shields.io/badge/lang-en-blue.svg)](README.md)
[![de](https://img.shields.io/badge/lang-de-yellow.svg)](README.de.md)

# ESPHome ↔ L&S Emotion "Bielefeld" (CC2500)

Control the **older L&S Emotion lighting with the "Bielefeld" control unit** – as fitted
for example in Hülsta furniture – from ioBroker, Home Assistant or any other system that
talks to ESPHome. No gateway, no changes to the remote or the control unit: an ESP32 with a
**CC2500** radio module (2.4 GHz) speaks the remote's protocol.

The newer **"Frankfurt"** series uses a different radio protocol and is covered by
[SergeyZh/emotion-control-ha](https://github.com/SergeyZh/emotion-control-ha). This project
started from it and documents the Bielefeld protocol, which is not compatible.

**Status: working**, tested with one 8-button remote (PCB `TxD8 LUS_RC24`, 2016) and an
"Emotion 4-Kanal Steuerung Bielefeld 12/24 V" with three lamps connected.

## Features

- **Lamps as lights:** each channel (1–4) appears as an on/off light.
- **All on / all off, warmer / colder** as buttons.
- **Dimming:** "hold key" buttons with a configurable hold time, also for several channels at once.
- **Remote sync:** keys pressed on the real remote are received; the lamp states follow and the
  pressed key is shown as a text sensor.
- **Discovery mode:** without a configured ID the component only listens and logs the ID of
  every remote it hears.
- **Diagnostic tools** used to find the protocol: a 2.4 GHz spectrum scanner and a raw capture
  that does not need to know the sync word ([docs/TOOLS.md](docs/TOOLS.md)).

## Which remote do I have?

| | Bielefeld (this project) | Frankfurt ([emotion-control-ha](https://github.com/SergeyZh/emotion-control-ha)) |
|---|---|---|
| Remote | 8 buttons: sun, snowflake, 1–4, on (\|), off (O) | rotary wheel or 3 buttons |
| Radio | 2433.0 MHz, 2-FSK, ~10 kBaud | 2436.2 MHz, MSK, 250 kBaud |
| Microcontroller in remote | STM8L101 | PIC16F1503 |

Not sure? Flash the [spectrum scanner](docs/TOOLS.md) and press a button: Bielefeld shows up
around 2433 MHz.

## What you need

| Part | Notes |
|---|---|
| **ESP32** | any board; tested with an ESP32 DevKit (ESP-WROOM-32) |
| **CC2500 module** | 2.4 GHz, **26 MHz crystal**. Modules with a 2×5 pin header (2.54 mm) need no soldering |
| **Your L&S remote** | its ID is what the ESP32 uses (it acts as a second copy of your remote) |

Power the module from **3.3 V only**.

### Wiring (CC2500 module → ESP32)

| Module | Signal | ESP32 |
|---|---|---|
| SCLK | SPI clock | GPIO18 |
| SI | MOSI | GPIO23 |
| SO | MISO | GPIO19 |
| CSN | chip select | GPIO5 |
| VCC / GND | power | 3V3 / GND |

GDO0 and GDO2 are not needed.

## Quick start

1. Copy the `components/` folder next to your ESPHome YAML.
2. **Find your remote's ID:** flash [`examples/find-remote-id.yaml`](examples/find-remote-id.yaml),
   press a key on the remote and look for
   `Remote found - remote_id: 12 34 56 78 9A BC DE F0: Ein (counter 530, -46 dBm)`.
3. Put that ID into [`examples/bielefeld.yaml`](examples/bielefeld.yaml) (native API, e.g. Home
   Assistant / ioBroker ESPHome adapter) or [`examples/bielefeld-mqtt.yaml`](examples/bielefeld-mqtt.yaml)
   (MQTT, e.g. ioBroker MQTT adapter) and flash it.
4. After flashing, press **"All off"** once so the ESP32 knows the lamp states.

## Minimal configuration

```yaml
external_components:
  - source: github://fewall/emotion-control-bielefeld
    components: [ls_bielefeld]

spi:
  clk_pin: GPIO18
  mosi_pin: GPIO23
  miso_pin: GPIO19

ls_bielefeld:
  cs_pin: GPIO5
  remote_id: "12 34 56 78 9A BC DE F0"   # your remote's ID

light:
  - platform: ls_bielefeld
    name: "Cabinet channel 1"
    channel: 1

button:
  - platform: ls_bielefeld
    name: "All off"
    key: aus
  - platform: ls_bielefeld
    name: "Dim channel 1"
    key: kanal_1
    hold: 2s
```

### Options

| Block | Option | Default | Meaning |
|---|---|---|---|
| `ls_bielefeld` | `remote_id` | – | 8-byte remote ID. Without it: listen only, log IDs |
| | `frequency` | `2433.0MHz` | carrier frequency |
| | `deviation` | `38kHz` | transmit FSK deviation |
| | `repeats` | `6` | copies of each frame on air |
| `light` | `channel` | – | lamp channel 1–4 |
| `button` | `key` | – | `sonne`, `schneeflocke`, `kanal_1` … `kanal_4`, `ein`, `aus`, or a list of keys pressed together |
| | `hold` | `0ms` | 0 = short press, otherwise how long the key is held (max. 10 s) |
| `text_sensor` | – | – | key currently pressed on the real remote, empty when released |

Key names are German, as printed on the remote (`sonne` = sun/warmer, `schneeflocke` =
snowflake/colder, `ein` = on, `aus` = off, `kanal` = channel).

## How the control unit behaves

- **Channel key, short:** toggles that lamp. **Held:** dims; the direction reverses with every hold.
  Full range takes about 2 s. Lamps keep a minimum brightness and remember it when switched off.
- **On / Off:** always all lamps.
- **Sun / snowflake:** colour temperature of all lamps.
- The control unit does **not** check the rolling counter strictly: after the ESP32 sent, the real
  remote keeps working.

## Limitations

- **No feedback from the control unit.** Lamp states are an estimate based on everything the ESP32
  sends and receives. If it is wrong (power cut, missed frame), "All off" re-syncs it.
- **Brightness is not a slider yet:** the dimming direction alternates, so only relative
  "hold" buttons are offered.
- The transmit deviation (38 kHz) is TI's SmartRF default for 10 kBaud and works here, but was not
  measured on the original remote.

## Documentation

| Document | Contents |
|---|---|
| [docs/PROTOCOL.md](docs/PROTOCOL.md) | radio parameters, frame format, keys, counter, hold sequence |
| [docs/TOOLS.md](docs/TOOLS.md) | spectrum scanner, raw capture, decoder – how the protocol was found |

## Credits

- [SergeyZh/emotion-control-ha](https://github.com/SergeyZh/emotion-control-ha) and
  [emotion-control-re](https://github.com/SergeyZh/emotion-control-re) – the Frankfurt protocol,
  CC2500 driver structure and the idea that made this possible.

## License

[Apache License 2.0](LICENSE).

This project is not affiliated with or endorsed by L&S Deutschland GmbH or Hülsta. "Emotion",
"L&S" and "Hülsta" are trademarks of their owners and are used only to state compatibility.
