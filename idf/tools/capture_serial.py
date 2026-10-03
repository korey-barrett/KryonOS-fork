"""Read the board's serial output for a few seconds after a clean run-mode reset.

Why this is not just `idf.py monitor`: on the CP210x auto-reset circuit, *opening* the port asserts
DTR/RTS and the chip resets before you can read anything, and depending on the order those lines
settle it can land in the ROM bootloader instead of flashing the app (a blank panel with a dark
backlight is that symptom, not a firmware fault). So the reset is driven explicitly -- IO0 released,
EN pulsed -- and the port is read from that instant, which is the only way to be sure the banner
being printed is the one after a reset this script caused.
"""

import sys
import time

import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM5"
SECONDS = float(sys.argv[2]) if len(sys.argv) > 2 else 12.0

ser = serial.Serial()
ser.port = PORT
ser.baudrate = 115200
ser.timeout = 0.2
ser.dtr = False
ser.rts = False
ser.open()

# EN low then high = reset then run; IO0 stays released so it boots the app, not the ROM loader.
ser.setRTS(True)
time.sleep(0.15)
ser.setRTS(False)
time.sleep(0.05)

deadline = time.time() + SECONDS
while time.time() < deadline:
    data = ser.read(4096)
    if data:
        sys.stdout.write(data.decode("utf-8", "replace"))
        sys.stdout.flush()

ser.close()
print("\n--- capture finished ---")
