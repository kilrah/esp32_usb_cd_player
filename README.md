# USB CD Player (ESP32-S3)

A standalone audio CD player. It reads audio CDs from a USB optical drive connected to the ESP32-S3
USB Host port, plays them through a PCM5102 I2S DAC, and shows track info on the LilyGO T-Display S3's screen.

The project started from Espressif's USB Host MSC example. The MSC driver has been replaced by
the custom [`usb_host_atapi`](components/usb_host_atapi/README.md) component.

## Status

| Area | State |
|---|---|
| Drive detection, INQUIRY, disc presence polling | Working |
| TOC (track list, track types, pre-emphasis flag) | Working |
| CD-TEXT (album/track titles, artists, UPC, ISRC) | Working |
| Raw CD-DA audio reads (READ CD) | Working, ~3.8x realtime (~650 KiB/s) max; drive set to 4x |
| Eject / load, error recovery (STALL, timeouts, BOT reset) | Working |
| I2S output to PCM5102 | Working: gapless across tracks, 0 underruns in testing. Needs the module's solder bridges set (see below) |
| Transport: play/pause, stop, next/prev, seek, track select, volume | Working (serial commands; physical buttons to be added) |
| Display UI (album/artist/title, track, time, progress, state, volume, buffer) | Working |
| Diagnostics (I2S pin frequency check, test tone, slow/failed read logging) | Working |
| De-emphasis for pre-emphasis tracks | Not implemented |
| Non-ASCII CD-TEXT characters on screen | Not rendered (LVGL Montserrat fonts are ASCII only) |
| Final board: partition table, flash QIO mode | **Pending.** The app uses ~650 KB of the 1 MB partition |

Verified with an **HP BD ROM BC-5501H** (slim SATA BD drive) behind an **Initio** USB bridge
(VID 0x13fd, PID 0x0840, interface subclass 0x02 / ATAPI, Bulk-Only), using a pressed audio CD with
CD-TEXT (Lorde, *Virgin*, 11 tracks).

## Hardware (current prototype)

**Board: LilyGO T-Display S3** (non-touch; ESP32-S3R8, 16 MB flash, 8 MB octal PSRAM, chip rev v0.2,
1.9" 320x170 ST7789 LCD on an 8-bit parallel bus). The board has no USB-serial converter, so an external FTDI
adapter is used for the console.

| Item | GPIO / detail |
|---|---|
| USB Host | Native USB-C connector (GPIO19/20), full speed only (12 Mbit/s). The drive is plugged in here |
| Console | External FTDI on **COM23**: board **TX = GPIO16**, **RX = GPIO21**, 115200 baud. The pin silkscreen is cut off and misleading (it looks like "1/2") |
| PCM5102 DAC | **LCK = GPIO43**, **DIN = GPIO44**, **BCK = GPIO18**, **SCK (MCLK) = GPIO17** |
| Onboard buttons | GPIO0 (BOOT) and GPIO14. Currently unused |
| LCD | ST7789 i80: D0-D7 = GPIO39-42, 45-48; WR 8, RD 9, DC 7, CS 6, RST 5, power 15, backlight 38 |
| Battery ADC | GPIO4 (not used) |
| Reserved pins | GPIO33-37 are used by the octal PSRAM |
| Flashing | **Only through the native USB port in download mode** (see below). The FTDI on GPIO16/21 cannot flash: the ROM bootloader only uses GPIO43/44 or native USB |
| Drive power | Needs a 5 V supply that can deliver the spin-up current. **Don't use a power bank**: power banks short the USB data lines to signal fast-charge capability, which breaks the connection to the drive |

GPIO43 is also the ROM's default UART TX, so the DAC's LCK line toggles briefly during boot. This is harmless.

Earlier prototype: an ESP32-S3 N16R8 devkit (UART console on COM22, WS2812 status LED on GPIO48). Its fuse on the
host connector's VBUS line dropped too much voltage and browned out the drive during spin-up; it had to be bypassed.
The WS2812 status LED code was removed when the display took over status indication.

## Building and flashing

ESP-IDF **v6.1** is installed with the Espressif Installation Manager (EIM).
`C:\esp\v6.1\esp-idf\export.ps1` does **not** work on this machine (it looks for a Python env that
doesn't exist). Activate the environment with the EIM profile script instead:

```powershell
. C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1
idf.py build
idf.py -p COM17 flash        # native USB in download mode, see below
idf.py -p COM23 monitor      # logs + serial commands via the FTDI adapter
```

**Flashing the T-Display S3:** the running firmware uses the native USB port as USB Host, so no
serial port is exposed there. To flash:

1. Unplug the drive and connect the native USB-C port to the PC.
2. Hold **BOOT**, press and release **RST**, then release BOOT. The ROM's USB serial port appears
   (VID 0x303A, PID 0x1001; it was **COM17**).
3. Run `idf.py -p COM17 flash`.
4. **Press RST.** esptool's hard reset at the end of flashing did not reliably leave download mode, and the
   board stayed in the bootloader.
5. Reconnect the drive. Logs are on COM23.

Dependencies (managed components): `espressif/usb` (USB Host library, external since IDF 6.0),
`espressif/esp_lvgl_port`, `lvgl/lvgl` (v9).

### Configuration (`sdkconfig.defaults`)

- `CONFIG_USB_HOST_HUBS_SUPPORTED=y`: a powered USB hub can be placed between the board and the drive.
- 16 MB flash, octal PSRAM at 80 MHz. `malloc()` puts allocations of 16 KB or more in PSRAM. USB transfer
  buffers are allocated by the USB library from internal DMA-capable RAM.
- Console on UART0, TX GPIO16 / RX GPIO21; no secondary console. The native USB port is the USB Host.
- LVGL: Montserrat 14/20/28 fonts; clib malloc/string/sprintf, so large LVGL allocations can go to PSRAM.
- Not yet changed: the partition table (single 1 MB factory app, ~37% free) and flash mode (DIO). These will be set for the final board.

## Application

| File | Role |
|---|---|
| [main/cd_player_main.c](main/cd_player_main.c) | Drive connect/disconnect, disc polling (TEST UNIT READY every 1 s), TOC/CD-TEXT loading, eject/load, serial commands |
| [main/player.c](main/player.c) | Playback engine: a reader task fills 12 × 16-sector chunks (~2.5 s, 450 KB, `NUM_CHUNKS`) in PSRAM; a writer task streams them to I2S in 2-sector slices, applies the volume and tracks the position and the buffered sector count. A generation counter drops stale chunks on seek/stop/unload. Plays continuously across tracks, skips data tracks, stops at the lead-out. Reads are retried 3×; slow (>500 ms) and failed reads are logged |
| [main/player.h](main/player.h) | Player API, status snapshot, volume cap (`PLAYER_VOLUME_MAX`, `PLAYER_VOLUME_STEP`, `player_volume_display()`) |
| [main/audio_out.c](main/audio_out.c) | I2S0 master, 44.1 kHz, 16-bit stereo, Philips format, MCLK = 256·fs. DMA auto-clear outputs silence on pause or underrun. Pin check and test-tone diagnostics |
| [main/ui.c](main/ui.c) | LVGL screen, refreshed every 200 ms from a `player_get_status()` snapshot (runs in the LVGL task) |
| [components/tdisplay_s3](components/tdisplay_s3) | ST7789 i80 + LVGL port setup, ported from [hiruna/esp-idf-t-display-s3](https://github.com/hiruna/esp-idf-t-display-s3) (MIT) to IDF 6 |

On disc load the drive is set to 4x (`SET CD SPEED`) for less noise. CD audio needs 1x.
A newly loaded disc starts in **stopped** state; press play.

### Controls

For now the player is controlled only through serial commands. The onboard buttons are not used; dedicated
buttons will be added later. An earlier, uncommitted version used the two onboard buttons through the
`espressif/button` component (click / double click / hold events), which worked well for this.

### Serial commands (COM23, 115200)

| Key | Action |
|---|---|
| `p` | Play / pause |
| `s` | Stop |
| `n` / `b` | Next / previous track |
| `f` / `r` | Seek +10 s / -10 s |
| `+` / `-` | Volume ±10 % as displayed (0-100 %, where 100 % = the internal cap; see below) |
| `e` | Eject / load |
| `<number>` Enter | Play track number |
| `i` | Print status |
| `a` | Diagnostics: measure MCLK/BCK/LCK frequencies and DIN activity on the I2S pins (pulse counter, outputs undisturbed) |
| `t` | Diagnostics: 1 kHz test tone for 3 s (pauses the player) |
| `h` | Help |

Every command prints a status line, for example:

```
STATUS state=playing track=8/11 pos=00:26/02:35 vol=100 buf=95%(2453ms) underruns=0 read_errors=0 title="GRWM"
```

[tools/sercmd.py](tools/sercmd.py) sends timed commands and captures the output without toggling DTR/RTS.
For example, play, then print the status after 5 s:

```powershell
python tools/sercmd.py COM23 6 "0.5:p" "5:i"     # '~' in a command is sent as Enter, e.g. "1:7~" = play track 7
```

The volume is capped internally at `PLAYER_VOLUME_MAX` = 50 (`player.h`), which is -12 dB with the quadratic gain curve
(gain = (v/100)²). The display and the status line scale it so that the cap shows as **100 %**; one step
(`PLAYER_VOLUME_STEP` = 5) is 10 % on the display. Playback starts at the cap. The PCM5102 outputs about 2.1 V RMS at full
scale, which overdrives the downstream amplifier; the digital volume itself only attenuates.

### PCM5102 module setup

The common purple PCM5102A modules have four solder-bridge pads on the back, each to be bridged to H or L.
**Without them the DAC stays silent**, even though all I2S signals are present.

| Pad | Function | Setting |
|---|---|---|
| 1 | FLT (filter) | L (normal latency) |
| 2 | DEMP (de-emphasis) | L (off) |
| 3 | XSMT (soft mute) | **H (unmuted)** |
| 4 | FMT (format) | L (I2S) |

SCK is driven with MCLK = 256·fs from GPIO17. Alternatively, tie SCK to GND to use the DAC's internal PLL, and set
`.mclk = I2S_GPIO_UNUSED` in `audio_out.c`. The output is line level; headphones work, but quietly.

### Screen

```
+------------------------------------------------+
| Lorde - Virgin                       (scrolls) |
| GRWM                                 (scrolls) |
| Track 8 / 11                                > |
| 00:26               / 02:35                    |
| [=========----------------------------------] |
| Vol 100%  Buf 91%                       34:56 |
+------------------------------------------------+
```

When there is no disc, the title line shows the drive state: Waiting for drive, Drive found..., Loading...,
No disc, Tray open, Reading disc..., Cannot read disc, Drive error.

## Known issues / gotchas

- **Resetting the T-Display S3 remotely isn't possible.** The FTDI has no DTR/RTS wiring, and the native port is
  in USB Host mode. Press RST to see the boot log on COM23. The serial commands work without a reset.
- **GPIO0 glitch from DTR/RTS** (old N16R8 devkit only): its auto-program circuit pulsed GPIO0 when the UART
  port was opened, which ejected the disc. The FTDI on the T-Display has no DTR/RTS wiring, so this doesn't apply there.
  When buttons are added, debounce them, and avoid GPIO0 if a programming circuit can toggle it.
- **First contact after an ESP reset** (drive stays powered) can take a while if the drive is busy.
  INQUIRY retries every 3 s for up to ~60 s and logs `Drive not responding yet`.
- **Spin-up latency.** The first read after the disc spins down takes ~3 s, so play starts with a short delay.
- Throughput is limited by the ESP32-S3's full-speed USB to ~650 KiB/s. Audio needs 172 KiB/s, which
  leaves enough headroom for read-ahead and retries.
- Read errors are retried 3 times, then the chunk is replaced with silence. The count shows as `read_errors`,
  and a warning icon appears on screen. Every failed attempt, and every read slower than 500 ms, is logged
  (`player: Slow read at LBA ...`); normal reads take ~60 ms.
- **Buffer metric** (`Buf` on screen, `buf=..%(..ms)` in the status line): audio read ahead but not yet sent to
  I2S, counted in sectors. Full is ~90-97 % (~2.4 s) during playback, because the reader always has one chunk in
  flight. When the disc stops being readable, the value stays up for a while: the drive keeps serving reads from
  its own read-ahead cache. Then it falls to zero within ~2.5 s, once that cache runs dry.

## Next steps

1. De-emphasis filter for tracks flagged with pre-emphasis (rare on commercial CDs).
2. A UTF-8 font with Latin-1 glyphs for CD-TEXT (accents).
3. Media change events via `atapi_host_get_media_status()` instead of polling TEST UNIT READY every second.
4. Power management: spin down / stop the drive after a while in stopped state, dim the backlight.
5. Physical buttons (transport + eject), with debouncing.
6. Final board: partition table (larger app partition; the app is already 650 KB), QIO flash.
7. Optional: a larger read-ahead buffer (`NUM_CHUNKS`; 10 s ≈ 1.8 MB of PSRAM) if longer read interruptions
   need to be bridged. The drive's own cache already covers several seconds.
