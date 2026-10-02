#!/usr/bin/env python3
"""Capture a full ESP32 boot log from the first line.

Why this exists: boot-log.ps1 only attaches to an already-running device, so it
misses the first boot lines (App version / role / probe-enabled messages). This
script pulses RTS itself to hard-reset the chip and then reads from the very
first byte, which is what you need to verify boot-time diagnostics.

Usage:
    <idf-python> tools/capture-boot.py --port COM3 --seconds 30 [--out file.log]
"""
import argparse
import sys
import time

import serial


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--port', required=True)
    ap.add_argument('--seconds', type=int, default=30)
    ap.add_argument('--baud', type=int, default=115200)
    ap.add_argument('--out')
    args = ap.parse_args()

    sp = serial.Serial(args.port, args.baud, timeout=0.2)
    # Hard reset: esptool-style RTS/DTR toggling on the USB-serial bridge.
    sp.setDTR(False)
    sp.setRTS(True)
    time.sleep(0.15)
    sp.setRTS(False)
    time.sleep(0.05)
    sp.reset_input_buffer()

    lines = []
    deadline = time.time() + args.seconds
    while time.time() < deadline:
        raw = sp.readline()
        if not raw:
            continue
        text = raw.decode('utf-8', errors='replace').rstrip('\r\n')
        lines.append(text)
        print(text, flush=True)
    sp.close()

    if args.out:
        with open(args.out, 'w', encoding='utf-8') as fh:
            fh.write('\n'.join(lines) + '\n')
    return 0


if __name__ == '__main__':
    sys.exit(main())
