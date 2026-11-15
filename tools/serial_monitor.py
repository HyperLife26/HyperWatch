#!/usr/bin/env python3
"""Read HyperWatch telemetry from the ESP32-S3 USB-Serial-JTAG port.

Why this exists: `arduino-cli monitor` (and some terminal programs) assert
DTR/RTS when they open the port. On the ESP32-S3 SuperMini's native USB that
can hold the chip in reset, so the port opens but no telemetry appears. This
reader explicitly deasserts both lines after opening.

Usage:
    python3 tools/serial_monitor.py -p /dev/ttyACM0 -b 115200
"""

import argparse
import sys

import serial


def main() -> int:
    parser = argparse.ArgumentParser(description="HyperWatch serial reader")
    parser.add_argument("-p", "--port", default="/dev/ttyACM0")
    parser.add_argument("-b", "--baud", type=int, default=115200)
    args = parser.parse_args()

    port = serial.Serial()
    port.port = args.port
    port.baudrate = args.baud
    port.timeout = 0.2
    # Set the lines before open so the driver applies them immediately.
    port.dtr = False
    port.rts = False
    port.open()
    port.dtr = False
    port.rts = False
    port.reset_input_buffer()

    try:
        while True:
            data = port.read(1024)
            if data:
                sys.stdout.write(data.decode("utf-8", "replace"))
                sys.stdout.flush()
    except KeyboardInterrupt:
        pass
    finally:
        port.close()

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
