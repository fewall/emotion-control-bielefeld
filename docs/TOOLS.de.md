[![en](https://img.shields.io/badge/lang-en-blue.svg)](TOOLS.md)
[![de](https://img.shields.io/badge/lang-de-yellow.svg)](TOOLS.de.md)

# Diagnose-Werkzeuge

Zwei ESPHome-Komponenten und ein Skript, mit denen das Bielefeld-Protokoll gefunden wurde. Sie
brauchen nur den ESP32 mit CC2500-Modul (Verkabelung wie im [README](../README.de.md)) und können
auch bei anderen 2,4-GHz-Geräten mit CC2500 und unbekannten Einstellungen helfen. Immer **nur eine**
CC2500-Komponente gleichzeitig verwenden – alle nutzen denselben Chip.

## 1. Spektrumscanner – `cc2500_scanner`

Läuft in 250 Schritten zu 333 kHz über 2400–2483 MHz, misst die Signalstärke (RSSI) mit einem
812-kHz-Empfangsfilter und schreibt alle paar Sekunden ein Spitzenwert-Spektrum ins Log. Schmale
Signale, die sich deutlich von ihrer Umgebung abheben, werden zusätzlich aufgelistet (WLAN ist ca.
20 MHz breit, eine Fernbedienung nur einige hundert kHz).

Beispiel: [`examples/scanner.yaml`](../examples/scanner.yaml)

```
--- 6 sweeps in the last 5 s (total 108, timeouts 0) ---
  MHz |2400      2410      2420      2430      2440      2450      2460      2470      2480|
  pk  |=..+..................*...:=::==@@+--:::-...... .:..-.**+##+....##%.=+=..:%:..-.-.-.|
```

Skala: `' '` < −93 dBm … `'@'` ≥ −37 dBm, etwa 7 dB pro Zeichen. Das `@@` bei 2432–2433 MHz
erschien nur beim Tastendrücken – das war die Fernbedienung.

**Tipp:** Notiere dir, wann du Tasten drückst. Kurze WLAN- und Bluetooth-Pakete erzeugen ebenfalls
einzelne „narrow peaks“; die Fernbedienung ist das, was beim Drücken immer wieder an derselben Stelle
auftaucht.

| Option | Standard | Bedeutung |
|---|---|---|
| `report_interval` | `5s` | wie oft das Spektrum ins Log geschrieben wird |
| `dwell_time` | `300us` | Messzeit pro Kanal |

## 2. Rohmitschnitt – `cc2500_capture`

Lauscht auf einer Frequenz und löst auf die **Präambel** aus (32 Bit `1010…`, bis zu zwei Bitfehler)
statt auf ein Sync-Wort. Alles danach – das echte Sync-Wort und die Nutzdaten – landet als Hex im Log.
Da Modulation und Datenrate unbekannt sind, werden zehn Profile durchprobiert:

| # | Profil | | # | Profil |
|---|---|---|---|---|
| 0 | 250 kBaud MSK | | 5 | 100 kBaud GFSK |
| 1 | 250 kBaud 2-FSK | | 6 | 38,4 kBaud 2-FSK |
| 2 | 250 kBaud GFSK | | 7 | 38,4 kBaud GFSK |
| 3 | 500 kBaud MSK | | 8 | **10 kBaud 2-FSK** ← Bielefeld |
| 4 | 100 kBaud 2-FSK | | 9 | 2,4 kBaud 2-FSK |

Beispiel: [`examples/capture.yaml`](../examples/capture.yaml). Während alle Profile laufen (ca. 3½ Min.)
etwa jede Sekunde eine Taste drücken. Das richtige Profil liefert viele starke, sich wiederholende Zeilen:

```
>>> Profile 8/9: 10 kBaud 2-FSK (dev 19 kHz) @ 2433.000 MHz - press buttons on the remote now
[P8]  -33 dBm | D3 91 D3 91 0E 09 04 00 DD 00 00 12 34 56 78 9A BC DE F0 20 AE 3C 80 DA
[P8]  -33 dBm | B4 E4 74 E4 43 82 41 00 37 40 00 04 8D 15 9E 26 AF 37 BC 08 2B 8F 20 36
<<< Profile 8 finished: 231 captures (81 not logged)
```

Die zweite Zeile ist dasselbe Paket um zwei Bit verschoben – das Präambelmuster passt an jeder
geraden Bitposition. Einzelne schwache Zeilen in allen Profilen sind Rauschen.

| Option | Standard | Bedeutung |
|---|---|---|
| `frequency` | `2433.0MHz` | Empfangsfrequenz (aus dem Scanner übernehmen) |
| `profile` | `-1` | `-1` = alle nacheinander, `0`–`9` = festes Profil |
| `profile_time` | `20s` | Dauer pro Profil |

## 3. Decoder – `tools/decode_capture.py`

Richtet die mitgeschnittenen Zeilen auf das Sync-Wort `D3 91 D3 91` aus, prüft die CRC des CC2500
und gibt die Felder aus:

```
python3 tools/decode_capture.py esphome-log.txt --profile 8

02:32:28 P8  -47 dBm  key=01 (Sonne         ) counter=  508 seq=  0  id=12 34 56 78 9A BC DE F0  CRC ok
02:32:35 P8  -51 dBm  key=02 (Schneeflocke  ) counter=  509 seq=  0  id=12 34 56 78 9A BC DE F0  CRC ok
02:32:36 P8  -56 dBm  key=00 (released      ) counter=  509 seq=  1  id=12 34 56 78 9A BC DE F0  CRC ok
```

Für ein Gerät mit anderem Sync-Wort `SYNC_BITS` oben im Skript anpassen.
