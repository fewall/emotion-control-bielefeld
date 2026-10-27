[![en](https://img.shields.io/badge/lang-en-blue.svg)](TOOLS.md)
[![de](https://img.shields.io/badge/lang-de-yellow.svg)](TOOLS.de.md)

# Diagnostic tools

Two ESPHome components and a script that were used to find the Bielefeld protocol. They need
nothing but the ESP32 + CC2500 module (same wiring as in the [README](../README.md)) and may help
with other 2.4 GHz CC2500 devices whose settings are unknown. Run **only one** CC2500 component at a
time – they all use the same chip.

## 1. Spectrum scanner – `cc2500_scanner`

Sweeps 2400–2483 MHz in 250 steps of 333 kHz, measures the signal strength (RSSI) with an 812 kHz
receive filter and logs a peak-hold spectrum every few seconds. Narrow signals that rise clearly above
their surroundings are listed separately (Wi-Fi is ~20 MHz wide, a remote only a few hundred kHz).

Example: [`examples/scanner.yaml`](../examples/scanner.yaml)

```
--- 6 sweeps in the last 5 s (total 108, timeouts 0) ---
  MHz |2400      2410      2420      2430      2440      2450      2460      2470      2480|
  pk  |=..+..................*...:=::==@@+--:::-...... .:..-.**+##+....##%.=+=..:%:..-.-.-.|
```

Scale: `' '` < −93 dBm … `'@'` ≥ −37 dBm, about 7 dB per character. The `@@` around 2432–2433 MHz
appeared only while keys were pressed – that was the remote.

**Tip:** note the times when you press keys. Short bursts from Wi-Fi/Bluetooth also produce single
"narrow peaks"; the remote is the one that shows up again and again at the same place while you press.

| Option | Default | Meaning |
|---|---|---|
| `report_interval` | `5s` | how often the spectrum is logged |
| `dwell_time` | `300us` | measuring time per channel |

## 2. Raw capture – `cc2500_capture`

Listens on one frequency and triggers on the **preamble** (32 bits `1010…`, up to two bit errors)
instead of a sync word. Whatever follows – the real sync word and the payload – is logged as hex.
Since modulation and data rate are unknown, it cycles through ten profiles:

| # | Profile | | # | Profile |
|---|---|---|---|---|
| 0 | 250 kBaud MSK | | 5 | 100 kBaud GFSK |
| 1 | 250 kBaud 2-FSK | | 6 | 38.4 kBaud 2-FSK |
| 2 | 250 kBaud GFSK | | 7 | 38.4 kBaud GFSK |
| 3 | 500 kBaud MSK | | 8 | **10 kBaud 2-FSK** ← Bielefeld |
| 4 | 100 kBaud 2-FSK | | 9 | 2.4 kBaud 2-FSK |

Example: [`examples/capture.yaml`](../examples/capture.yaml). Press a key about every second while
all profiles run (~3½ min). The right profile produces many strong lines that repeat:

```
>>> Profile 8/9: 10 kBaud 2-FSK (dev 19 kHz) @ 2433.000 MHz - press buttons on the remote now
[P8]  -33 dBm | D3 91 D3 91 0E 09 04 00 DD 00 00 12 34 56 78 9A BC DE F0 20 AE 3C 80 DA
[P8]  -33 dBm | B4 E4 74 E4 43 82 41 00 37 40 00 04 8D 15 9E 26 AF 37 BC 08 2B 8F 20 36
<<< Profile 8 finished: 231 captures (81 not logged)
```

The second line is the same frame shifted by two bits – the preamble pattern can match at any even
bit offset. Single weak lines in every profile are noise.

| Option | Default | Meaning |
|---|---|---|
| `frequency` | `2433.0MHz` | frequency to listen on (take it from the scanner) |
| `profile` | `-1` | `-1` = cycle through all, `0`–`9` = fixed profile |
| `profile_time` | `20s` | time per profile |

## 3. Decoder – `tools/decode_capture.py`

Re-aligns the captured lines on the sync word `D3 91 D3 91`, checks the CC2500 CRC and prints the
fields:

```
python3 tools/decode_capture.py esphome-log.txt --profile 8

02:32:28 P8  -47 dBm  key=01 (Sonne         ) counter=  508 seq=  0  id=12 34 56 78 9A BC DE F0  CRC ok
02:32:35 P8  -51 dBm  key=02 (Schneeflocke  ) counter=  509 seq=  0  id=12 34 56 78 9A BC DE F0  CRC ok
02:32:36 P8  -56 dBm  key=00 (released      ) counter=  509 seq=  1  id=12 34 56 78 9A BC DE F0  CRC ok
```

For a device with a different sync word, change `SYNC_BITS` at the top of the script.
