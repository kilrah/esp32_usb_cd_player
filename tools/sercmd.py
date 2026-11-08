"""Send timed single-key commands to the CD player console and print its output.

Usage:  python tools/sercmd.py PORT SECONDS [AT:TEXT ...]
        AT   = seconds after opening the port
        TEXT = characters to send; '~' is sent as Enter (newline)
Example: python tools/sercmd.py COM23 10 "0.5:7~" "1:p" "6:i"

DTR/RTS are kept low *before* opening the port: on boards with an auto-program
circuit, toggling them pulses EN/GPIO0 (this ejected the disc on the old devkit).
"""
import sys
import time

import serial

sys.stdout.reconfigure(encoding="utf-8", errors="replace")  # Windows console can't print all log bytes

port, secs = sys.argv[1], float(sys.argv[2])
plan = []
for arg in sys.argv[3:]:
    at, text = arg.split(":", 1)
    plan.append((float(at), text.replace("~", "\n")))
plan.sort(key=lambda p: p[0])

s = serial.Serial()
s.port, s.baudrate, s.timeout = port, 115200, 0.1
s.dtr = False
s.rts = False
s.open()
start = time.time()
while time.time() - start < secs:
    now = time.time() - start
    while plan and plan[0][0] <= now:
        _, text = plan.pop(0)
        sys.stdout.write(f"\n>>> send {text!r} at {now:.1f}s\n")
        s.write(text.encode())
    data = s.read(4096)
    if data:
        sys.stdout.write(data.decode(errors="replace"))
        sys.stdout.flush()
