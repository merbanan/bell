#!/usr/bin/env python3
"""Reset the module over DTR/RTS and capture what the ROM bootloader says.

The firmware itself prints nothing to UART0 - that pin pair is the payload the
bridge exists to carry - so the boot log is the only thing on the wire at
power-up, and it is the quickest way to confirm the chip is alive and that the
image flashed into it is loadable.

    python3 tools/capture_boot.py [port] [seconds]
"""

import sys
import time

import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyUSB1"
SECONDS = float(sys.argv[2]) if len(sys.argv) > 2 else 3.0

# The ESP8266 ROM loader talks at 74880 baud; the SDK follows at 115200.
BAUD = 74880

port = serial.Serial(PORT, BAUD, timeout=0.2)
port.setDTR(False)
port.setRTS(True)          # EN low: the chip is held in reset
time.sleep(0.1)
port.setRTS(False)         # release reset
print(f"reset {PORT}, listening for {SECONDS:g}s at {BAUD} baud\n")

deadline = time.time() + SECONDS
buf = bytearray()
while time.time() < deadline:
    chunk = port.read(port.in_waiting or 1)
    if chunk:
        buf += chunk

port.close()

print(buf.decode("utf-8", "replace").replace("\r\n", "\n").strip() or "(nothing received)")
