# USB CD Player (ESP32-S3)

Reads audio CDs from a USB optical drive connected to the ESP32-S3 USB Host port.
The end goal is a standalone CD player: audio goes out through an I2S DAC, and a display and buttons provide the user interface.

The project started from Espressif's USB Host MSC example. The MSC driver has been replaced by
the custom [`usb_host_atapi`](components/usb_host_atapi/README.md) component.

## Status

| Area | State |
|---|---|
| Drive detection, INQUIRY, disc presence polling | Working |
| TOC (track list, track types, pre-emphasis flag) | Working |
| CD-TEXT (album/track titles, artists, UPC, ISRC) | Working |
| Raw CD-DA audio reads (READ CD) | Working, ~3.8x realtime (~650 KiB/s) |
| Eject / load, error recovery (STALL, timeouts, BOT reset) | Working |
| Status LED (WS2812B) | Worked on the old N16R8 devkit; **disabled** on the T-Display S3 (no WS2812) |
| I2S DAC output | **Not started** |
| Display / buttons UI | **Not started** |
| Final board: partition table, flash QIO mode | **Pending.** The T-Display S3 is still a prototype setup |

Verified with an **HP BD ROM BC-5501H** (slim SATA BD drive) behind an **Initio** USB bridge
(VID 0x13fd, PID 0x0840, interface subclass 0x02 / ATAPI, Bulk-Only), using a pressed audio CD with
CD-TEXT (Lorde, *Virgin*, 11 tracks).

## Hardware (current prototype)

**Board: LilyGO T-Display S3** (ESP32-S3R8, 16 MB flash, 8 MB octal PSRAM, chip rev v0.2, 1.9" ST7789 LCD with an 8-bit parallel bus).
The board has no USB-serial converter, so an external FTDI adapter is used for the console.

| Item | Detail |
|---|---|
| USB Host | Native USB-C connector (GPIO19/20), full speed only (12 Mbit/s). The drive is plugged in here |
| Console (logs) | External FTDI adapter on **COM23**: board **TX = GPIO16**, **RX = GPIO21**, 115200 baud. The pin silkscreen is cut off and misleading (it looks like "1/2") |
| Flashing | **Only through the native USB port in download mode** (see below). The FTDI on GPIO16/21 cannot flash: the ROM bootloader only uses GPIO43/44 or native USB |
| Buttons | BOOT (**GPIO0**) = eject / load the tray. GPIO14 = second button, still unused |
| LCD | 8-bit parallel ST7789: D0-D7 = GPIO39-42, 45-48. **GPIO48 is LCD D7**, so the WS2812 status LED is disabled (`STATUS_LED_PIN -1` in `cd_player_main.c`) |
| Reserved pins | GPIO33-37 are used by the octal PSRAM |
| Drive power | Needs a 5 V supply that can deliver the spin-up current. **Don't use a power bank**: power banks short the USB data lines to signal fast-charge capability, which breaks the connection to the drive |

Earlier prototype: an ESP32-S3 N16R8 devkit (UART console on COM22, WS2812 on GPIO48). Its fuse on the
host connector's VBUS line dropped too much voltage and browned out the drive during spin-up; it had to be bypassed.

## Building and flashing

ESP-IDF **v6.1** is installed with the Espressif Installation Manager (EIM).
`C:\esp\v6.1\esp-idf\export.ps1` does **not** work on this machine (it looks for a Python env that
doesn't exist). Activate the environment with the EIM profile script instead:

```powershell
. C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1
idf.py build
idf.py -p COM17 flash        # native USB in download mode, see below
idf.py -p COM23 monitor      # logs via the FTDI adapter
```

**Flashing the T-Display S3:** the running firmware uses the native USB port as USB Host, so no
serial port is exposed there. To flash:

1. Unplug the drive and connect the native USB-C port to the PC.
2. Hold **BOOT**, press and release **RST**, then release BOOT. The ROM's USB serial port appears
   (VID 0x303A, PID 0x1001; it was **COM17**).
3. Run `idf.py -p COM17 flash`. esptool hard-resets the board into the new firmware at the end.
4. Reconnect the drive. Logs are on COM23.

Dependencies (managed components): `espressif/usb` (USB Host library, external since IDF 6.0) and
`espressif/led_strip`.

### Configuration (`sdkconfig.defaults`)

- `CONFIG_USB_HOST_HUBS_SUPPORTED=y`: a powered USB hub can be placed between the board and the drive
- 16 MB flash, octal PSRAM at 80 MHz. `malloc()` puts allocations of 16 KB or more in PSRAM. USB transfer
  buffers are allocated by the USB library from internal DMA-capable RAM.
- Not yet changed: the partition table (single 1 MB factory app) and flash mode (DIO). These will be set for the final board.

## Test application ([main/cd_player_main.c](main/cd_player_main.c))

1. Installs the USB Host library and the ATAPI driver.
2. When a drive is connected: installs it and prints vendor/product/revision.
3. Polls TEST UNIT READY every second and logs disc state changes (no disc, tray open, becoming ready).
4. When a disc is ready: reads the TOC and CD-TEXT, prints the track list, then reads 5 s of the first
   audio track and reports first-sector latency, throughput and peak/RMS levels.
5. BOOT button: eject / load.

Example output:

```
Album:  Virgin
Artist: Lorde
UPC/EAN: 0602478121333
TOC: tracks 1-11, total 34:56.10 (157210 sectors)
 #   type   start LBA   length
 1   audio          0   03:13.33   Hammer
 2   audio      14508   03:29.21   What Was That
 ...
I cd: First sector latency: 3045 ms        <- spin-up from standby
I cd: Read 375 sectors in 1321 ms: 651.6 KiB/s = 3.78x realtime
```

### Status LED ([main/status_led.c](main/status_led.c))

| LED | Meaning |
|---|---|
| 3 white flashes | Firmware started (after flash / reset) |
| Blue blink (1 per 1.5 s) | Waiting for USB drive |
| Yellow fast blink | Drive found / disc spinning up / tray moving |
| Yellow steady | Drive connected, no disc |
| Cyan steady | Reading TOC / audio |
| Green steady | Disc read OK |
| Red fast blink | Error (unusable device, command failure) |

USB link-level failures (e.g. `HUB: Root port reset failed`) never reach the app. They show up as
blue blinking that never ends.

## Known issues / gotchas

- **GPIO0 glitch from the serial port** (old N16R8 devkit). Its auto-program circuit pulled GPIO0 (BOOT)
  low briefly whenever something opened the UART port and toggled DTR/RTS, and the old firmware ejected the disc.
  The firmware now ignores presses shorter than 50 ms. On the T-Display S3 the FTDI only has TX/RX/GND
  connected, so this doesn't apply, but keep the filter in place. The final hardware should use dedicated button GPIOs, not GPIO0.
- **Resetting the T-Display S3 remotely isn't possible.** The FTDI has no DTR/RTS wiring, and the native port is
  in USB Host mode. Press RST to see the boot log on COM23. When the app is idle it logs only on state changes.
- **First contact after an ESP reset** (drive stays powered) can take a while if the drive is busy.
  INQUIRY retries every 3 s for up to ~60 s and logs `Drive not responding yet`.
- **Spin-up latency.** The first read after the disc spins down takes ~3 s.
- Throughput is limited by the ESP32-S3's full-speed USB to ~650 KiB/s. Audio needs 172 KiB/s, so
  there is enough headroom for buffering and re-reads, but not much for fast scanning.

## Next steps

1. I2S DAC output: a playback task with a PSRAM ring buffer fed by `atapi_host_read_cd_audio()`
   (2352-byte sectors = 588 stereo frames, 16-bit LE, 44.1 kHz, ready for I2S as-is).
   Consider de-emphasis for tracks flagged with pre-emphasis.
2. Player state machine: play / pause / stop / next / prev / seek, track and time display.
3. Display and buttons UI using CD-TEXT for titles.
4. Read robustness: retry/re-read on errors, possibly `SET CD SPEED` to reduce drive noise
   (`atapi_host_set_speed()`), media change events (`atapi_host_get_media_status()`).
5. Final board: partition table (larger app partition), QIO flash, button GPIOs.
