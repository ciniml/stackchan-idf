#!/usr/bin/env python3
"""Non-interactive serial log capture.

Resets the target via DTR/RTS (matching esptool's default-reset sequence),
then reads stdout from the device for a fixed duration. Useful from a
non-TTY harness where idf.py monitor refuses to run.

Usage:
    python tools/monitor_log.py [--port /dev/ttyACM0] [--seconds 8]
                                [--baud 115200]
"""
from __future__ import annotations

import argparse
import sys
import time

import serial


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="/dev/ttyACM0")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--seconds", type=float, default=8.0)
    args = ap.parse_args()

    with serial.Serial(args.port, args.baud, timeout=0.2) as ser:
        # esptool default-reset: DTR=RST, RTS=BOOT.
        ser.setDTR(False); ser.setRTS(True);  time.sleep(0.1)
        ser.setDTR(True);  ser.setRTS(False); time.sleep(0.1)
        ser.setDTR(False); ser.setRTS(False)

        end = time.time() + args.seconds
        while time.time() < end:
            chunk = ser.read(4096)
            if chunk:
                sys.stdout.buffer.write(chunk)
                sys.stdout.buffer.flush()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
