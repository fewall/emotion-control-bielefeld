#!/usr/bin/env python3
"""Decode cc2500_capture log lines into L&S Bielefeld frames.

The capture component triggers on the preamble (0xAAAAAAAA), so the bytes it logs
are shifted by an arbitrary (even) number of bits. This script searches the bit
stream for the doubled sync word D3 91 D3 91, re-aligns the bytes behind it, checks
the CC2500 hardware CRC and prints one line per distinct frame.

Usage:
    python3 decode_capture.py esphome-log.txt            # all profiles
    python3 decode_capture.py esphome-log.txt --profile 8
    python3 decode_capture.py esphome-log.txt --all      # also print repeated copies
"""
import argparse
import re

SYNC_BITS = "1101001110010001" * 2  # D3 91 D3 91
LINE_RE = re.compile(r"\[(\d\d:\d\d:\d\d)[^\]]*\].*\[P(\d+)\]\s+(-?\d+) dBm \| ([0-9A-Fa-f ]+)")
KEYS = ["Sonne", "Schneeflocke", "Kanal 1", "Kanal 2", "Kanal 3", "Kanal 4", "Ein", "Aus"]


def crc16(data: bytes, crc: int = 0xFFFF) -> int:
    """CC2500 hardware CRC: CRC-16, polynomial 0x8005, initial value 0xFFFF."""
    for byte in data:
        for i in range(8):
            feedback = ((crc >> 15) ^ (byte >> (7 - i))) & 1
            crc = ((crc << 1) & 0xFFFF) ^ (0x8005 if feedback else 0)
    return crc


def key_name(mask: int) -> str:
    if mask == 0:
        return "released"
    return " + ".join(name for bit, name in enumerate(KEYS) if mask & (1 << bit))


def realign(raw: bytes):
    """Return the bytes following the sync word, or None if it is not found."""
    bits = "".join(f"{b:08b}" for b in raw)
    pos = bits.find(SYNC_BITS)
    if pos < 0:
        # The capture may have started inside the first sync word.
        for skipped in range(1, 17):
            if bits.startswith(SYNC_BITS[skipped:]):
                pos = -skipped
                break
        else:
            return None
    rest = bits[pos + len(SYNC_BITS):]
    return bytes(int(rest[i:i + 8], 2) for i in range(0, len(rest) - 7, 8))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("logfile")
    parser.add_argument("--profile", type=int, help="only lines of this capture profile")
    parser.add_argument("--all", action="store_true", help="print every copy, not only changes")
    args = parser.parse_args()

    last = None
    total = good = 0
    with open(args.logfile, encoding="utf-8", errors="replace") as fh:
        for line in fh:
            m = LINE_RE.search(line)
            if not m:
                continue
            time, profile, rssi, hexdata = m.groups()
            if args.profile is not None and int(profile) != args.profile:
                continue
            frame = realign(bytes.fromhex(hexdata))
            if frame is None or len(frame) < 17 or frame[0] != 0x0E:
                continue
            total += 1
            crc_ok = crc16(frame[:15]) == (frame[15] << 8 | frame[16])
            good += crc_ok
            p = frame[1:15]
            current = (p[1], p[2], p[3], p[5], bytes(p[6:14]))
            if current == last and not args.all:
                continue
            last = current
            counter = p[2] << 8 | p[3]
            print(f"{time} P{profile} {int(rssi):4d} dBm  key={p[1]:02X} ({key_name(p[1]):<14}) "
                  f"counter={counter:5d} seq={p[5]:3d}  id={p[6:14].hex(' ').upper()}  "
                  f"{'CRC ok' if crc_ok else 'CRC BAD'}")
    print(f"\n{total} frames found, {good} with valid CRC")


if __name__ == "__main__":
    main()
