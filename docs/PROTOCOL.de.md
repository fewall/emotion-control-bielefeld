[![en](https://img.shields.io/badge/lang-en-blue.svg)](PROTOCOL.md)
[![de](https://img.shields.io/badge/lang-de-yellow.svg)](PROTOCOL.de.md)

# Funkprotokoll L&S Emotion „Bielefeld“

Ermittelt ausschließlich durch Mithören – die Firmware der Fernbedienung wurde **nicht** ausgelesen
(siehe [Wie es gefunden wurde](#wie-es-gefunden-wurde)). Alles hier wurde an 420
mitgeschnittenen Paketen geprüft; 419 davon bestanden die Hardware-CRC des CC2500.

## Hardware der Fernbedienung

| Teil | Hinweise |
|---|---|
| Platine | Aufdruck `TxD8 LUS_RC24`, `26.04.2016`, `V11` |
| Mikrocontroller | STMicroelectronics **STM8L101F3U6** (UFQFPN20, Aufdruck `L1S3`) |
| Funk | TI **CC2500**, 26-MHz-Quarz, Keramik-Chipantenne |
| Programmier-Pads | 4 Löcher, 1,27 mm Raster, neben dem Mikrocontroller: VDD, SWIM, GND, NRST (eckiges Pad = VDD) |
| Batterie | CR2032 |

## Funkparameter

| Parameter | Wert |
|---|---|
| Trägerfrequenz | **2433,0 MHz** (`FREQ = 0x5D93B1`, `CHANNR = 0`) |
| Modulation | **2-FSK** |
| Datenrate | **ca. 10 kBaud** (`DRATE_E = 8`, `DRATE_M = 0x93` → 9,99 kBaud) |
| Frequenzhub | nicht gemessen; 38 kHz (`DEVIATN = 0x44`) wird von der Steuerung akzeptiert |
| Präambel | `0xAA…` (mindestens 4 Byte) |
| Sync-Wort | **`D3 91`**, doppelt gesendet (`SYNC_MODE` 30/32 → `D3 91 D3 91` in der Luft) |
| Paket | variable Länge, erstes Byte = Länge = `0x0E` (14) |
| CRC | Hardware-CRC-16 des CC2500 (Polynom `0x8005`, Startwert `0xFFFF`) über Längenbyte + Nutzdaten |
| FEC / Whitening | aus |

Abgesehen von der Datenrate sind das die Standardwerte aus TIs SmartRF Studio für den CC2500 –
einschließlich der Grundfrequenz 2433 MHz, die auch die neuere Frankfurt-Serie nutzt.

Registersatz der Komponente `ls_bielefeld` (26-MHz-Quarz):

| Reg | Name | Wert | | Reg | Name | Wert |
|---|---|---|---|---|---|---|
| 0x04 | SYNC1 | `D3` | | 0x10 | MDMCFG4 | `88` (RX-Bandbreite 203 kHz) |
| 0x05 | SYNC0 | `91` | | 0x11 | MDMCFG3 | `93` |
| 0x06 | PKTLEN | `0E` | | 0x12 | MDMCFG2 | `03` (2-FSK, 30/32) |
| 0x07 | PKTCTRL1 | `0C` | | 0x13 | MDMCFG1 | `22` |
| 0x08 | PKTCTRL0 | `05` | | 0x15 | DEVIATN | `44` |
| 0x0B | FSCTRL1 | `08` | | 0x19 | FOCCFG | `16` |
| 0x0D–0x0F | FREQ | `5D 93 B1` | | 0x1A | BSCFG | `6C` |

## Paketaufbau

In der Luft: `Präambel | D3 91 D3 91 | 0E | Nutzdaten (14 Byte) | CRC (2 Byte)`

| Byte | Beispiel | Bedeutung |
|---|---|---|
| 0 | `09` | immer gleich (Pakettyp) |
| 1 | `40` | **Tasten-Bitmaske** (siehe unten), `00` = losgelassen |
| 2–3 | `02 12` | **Zähler**, 16 Bit, höherwertiges Byte zuerst, +1 pro Tastendruck |
| 4 | `00` | immer gleich |
| 5 | `00` | **Folgenummer** innerhalb eines Drucks: 0 = gedrückt, 1, 2, … beim Halten, letzte = losgelassen |
| 6–13 | `12 34 56 78 9A BC DE F0` | **ID der Fernbedienung** (fest pro Fernbedienung) |

### Tasten

| Bit | Taste | ESPHome `key` |
|---|---|---|
| `01` | Sonne (wärmer) | `sonne` |
| `02` | Schneeflocke (kälter) | `schneeflocke` |
| `04` | 1 | `kanal_1` |
| `08` | 2 | `kanal_2` |
| `10` | 3 | `kanal_3` |
| `20` | 4 | `kanal_4` |
| `40` | Ein (\|) | `ein` |
| `80` | Aus (O) | `aus` |

Gleichzeitig gehaltene Tasten setzen mehrere Bits, z. B. `0C` = Kanal 1 + Kanal 2.

### Ablauf eines Tastendrucks

Kurzer Druck auf „Ein“ (Zähler 530):

```
0E 09 40 02 12 00 00 12 34 56 78 9A BC DE F0 | 8E F4    gedrückt      (Folge 0, ca. 4–8× wiederholt)
0E 09 00 02 12 00 01 12 34 56 78 9A BC DE F0 | E8 D5    losgelassen   (Taste 00, Folge 1)
```

Kanal 1 ca. 3 s gehalten (Zähler 516):

```
0E 09 04 02 04 00 00 …   Folge 0  gedrückt
0E 09 04 02 04 00 01 …   Folge 1  weiter gehalten (ca. 0,5 s später)
…                         Folge 2 … 5, etwa alle 0,5 s eine Stufe
0E 09 00 02 04 00 06 …   Folge 6  losgelassen (Taste 00)
```

Jede Stufe wird mehrfach gesendet; ein Paket dauert in der Luft ca. 20 ms.

### Zähler

Der Zähler steigt pro Tastendruck um eins und wird von der Fernbedienung geführt. Die Steuerung
verwirft Pakete mit kleinerem Zähler **nicht**: Nachdem der ESP32 (ausgehend vom letzten Zähler der
Fernbedienung) gesendet hat, funktioniert die echte Fernbedienung weiter, obwohl ihr Zähler nun
zurückliegt. Die Komponente folgt trotzdem dem Zähler der Fernbedienung und speichert ihn im Flash.

## Verhalten der Steuerung (4-Kanal Bielefeld)

| Bedienung | Wirkung |
|---|---|
| Kanaltaste kurz | schaltet die Lampe dieses Kanals um |
| Kanaltaste gehalten | dimmt diese Lampe; beginnt ca. 0,5 s nach dem Drücken (mit dem 2. Paket); Richtung wechselt bei jedem Halten; ganzer Bereich ca. 1 s; bleibt oben ca. 3 s / unten ca. 1 s stehen und kehrt dann um; eine Mindesthelligkeit bleibt |
| mehrere Kanaltasten gehalten | dimmt diese Lampen gemeinsam |
| Ein / Aus | alle Lampen ein / aus; Helligkeit bleibt gespeichert |
| Sonne / Schneeflocke gehalten | Lichtfarbe aller Lampen, immer wärmer bzw. kälter bis zum Anschlag (keine Umkehr); ganzer Bereich ca. 1,2 s |

## Wie es gefunden wurde

1. **Sergeys Frankfurt-Komponente** hörte nichts – nicht einmal verworfene Pakete.
2. Die Fernbedienung wurde geöffnet: CC2500 + STM8L101. Das Auslesen der Firmware über SWIM
   scheiterte (die Debug-Schnittstelle antwortete nicht), daher ging es rein über Funk weiter.
3. Ein **Spektrumscanner** (`cc2500_scanner`) zeigte beim Tastendrücken ein starkes, wiederkehrendes
   Signal um **2433 MHz**.
4. Ein **Rohmitschnitt** (`cc2500_capture`) löste auf die Präambel aus (`0xAAAAAAAA`, 30/32 Bit) statt
   auf ein Sync-Wort und probierte zehn Modulations-/Datenraten-Profile durch. Nur
   **„10 kBaud 2-FSK“** lieferte Pakete – Hunderte, mit −33 dBm, und mit `D3 91 D3 91 0E` darin.
5. Nach dem Ausrichten des Bitstroms auf `D3 91 D3 91` ([tools/decode_capture.py](../tools/decode_capture.py))
   bestanden die Pakete die CRC des CC2500 (419 von 420). Tastendrücke in fester Reihenfolge ergaben die Bitbelegung.

Die Werkzeuge sind in [TOOLS.de.md](TOOLS.de.md) beschrieben und können bei anderen Varianten helfen.
