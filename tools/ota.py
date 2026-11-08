"""Upload a firmware image to the CD player over Wi-Fi and wait until it runs.

Usage:  python tools/ota.py [HOST] [IMAGE]
        HOST  = hostname or IP (default: cdplayer.local)
        IMAGE = app image (default: build/usbhost_cd.bin)
Example: idf.py build && python tools/ota.py
         python tools/ota.py 192.168.1.42

The device checks that the image belongs to this project, writes it to the inactive OTA slot and
reboots. This script then polls GET /info until the device reports the uploaded image's ELF SHA-256.
If the new firmware doesn't come back on Wi-Fi, the bootloader rolls back to the old one at the next reset.
"""
import http.client
import json
import os
import socket
import sys
import time

APP_DESC_OFFSET = 24 + 8    # esp_image_header_t + esp_image_segment_header_t
APP_DESC_MAGIC = 0xABCD5432

host = sys.argv[1] if len(sys.argv) > 1 else "cdplayer.local"
image = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(__file__), "..", "build", "usbhost_cd.bin")

with open(image, "rb") as f:
    data = f.read()

desc = data[APP_DESC_OFFSET:APP_DESC_OFFSET + 256]
if int.from_bytes(desc[0:4], "little") != APP_DESC_MAGIC:
    sys.exit(f"{image}: no app descriptor, not an app image")
version = desc[16:48].split(b"\0")[0].decode()
built = (desc[96:112].split(b"\0")[0] + b" " + desc[80:96].split(b"\0")[0]).decode()
elf_sha = desc[144:176].hex()

# Resolve once: mDNS lookups on Windows can take seconds, and the address is reused after the reboot
try:
    addr = socket.gethostbyname(host)
except OSError as e:
    sys.exit(f"Cannot resolve {host}: {e}")


def get_info(timeout=3):
    conn = http.client.HTTPConnection(addr, 80, timeout=timeout)
    try:
        conn.request("GET", "/info")
        resp = conn.getresponse()
        return json.loads(resp.read())
    finally:
        conn.close()


try:
    info = get_info()
except OSError as e:
    sys.exit(f"{host} ({addr}) not reachable: {e}")
print(f"Device {host} ({addr}): {info['version']} built {info['date']} {info['time']}, "
      f"running {info['partition']} ({info['ota_state']})")
print(f"Image  {image}: {version} built {built}, {len(data) / 1024:.0f} KiB")


def same_image(info):
    # older firmware reported only the first 9 hex digits
    return len(info["elf_sha256"]) >= 8 and elf_sha.startswith(info["elf_sha256"])


if same_image(info):
    print("Device already runs this image; uploading anyway")

# The device erases the target area before reading the body: allow for that in the timeout
conn = http.client.HTTPConnection(addr, 80, timeout=60)
conn.putrequest("POST", "/ota")
conn.putheader("Content-Type", "application/octet-stream")
conn.putheader("Content-Length", str(len(data)))
conn.endheaders()
t0 = time.time()
CHUNK = 16 * 1024
tty = sys.stdout.isatty()
last = -1
for off in range(0, len(data), CHUNK):
    conn.send(data[off:off + CHUNK])
    pct = min(100, (off + CHUNK) * 100 // len(data))
    if tty:
        print(f"\rUploading {pct:3d} %", end="", flush=True)
    elif pct // 25 != last:
        last = pct // 25
        print(f"Uploading {pct:3d} %", flush=True)
resp = conn.getresponse()
body = resp.read().decode(errors="replace").strip()
dt = time.time() - t0
print(f"{chr(13) if tty else ''}{resp.status} {body} ({dt:.1f} s, {len(data) / 1024 / dt:.0f} KiB/s)")
if resp.status != 200:
    sys.exit(1)

print("Waiting for the device to come back", end="", flush=True)
time.sleep(3)
deadline = time.time() + 60
while time.time() < deadline:
    try:
        info = get_info(timeout=2)
        break
    except (OSError, ValueError):
        print(".", end="", flush=True)
        time.sleep(1)
else:
    sys.exit("\nDevice did not come back within 60 s (no Wi-Fi? it rolls back to the old firmware on the next reset)")

print(f"\nDevice runs {info['version']} built {info['date']} {info['time']} from {info['partition']} "
      f"({info['ota_state']})")
if not same_image(info):
    sys.exit("ELF SHA-256 differs from the uploaded image: the update did not take effect (rolled back?)")
print("Update OK")
