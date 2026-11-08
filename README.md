# USB CD Player (ESP32-S3)

A standalone audio CD player. It reads audio CDs from a USB optical drive connected to the ESP32-S3
USB Host port, plays them through a PCM5102 I2S DAC, and shows track info on the LilyGO T-Display S3's screen.

The project started from Espressif's USB Host MSC example. The MSC driver has been replaced by
the custom [`usb_host_atapi`](components/usb_host_atapi/README.md) component.

## Status

| Area | State |
|---|---|
| Drive detection, INQUIRY, disc presence polling | Working: media events (GESN) when the drive reports them, else TEST UNIT READY. Tray locked while a disc is loaded (GESN drives), the eject button stops playback cleanly |
| TOC (track list, track types, pre-emphasis flag) | Working |
| CD-TEXT (album/track titles, artists, UPC, ISRC) | Working |
| Raw CD-DA audio reads (READ CD) | Working, ~3.8x realtime (~650 KiB/s) max; drive set to 4x |
| Eject / load, error recovery (STALL, timeouts, BOT reset) | Working |
| I2S output to PCM5102 | Working: gapless across tracks, 0 underruns in testing. Needs the module's solder bridges set (see below) |
| Transport: play/pause, stop, next/prev, seek, track select, volume | Working (web UI and serial commands; physical buttons to be added). Autoplay on disc load (setting); volume and autoplay persisted in NVS |
| Web UI (controls, track list, settings, firmware upload) at http://cdplayer.local/ | Working |
| Setup access point (no / unreachable Wi-Fi network) with captive portal | Working: AP, scan, web UI over the LAN tested; joining the AP from a phone not yet tested |
| Album info + cover art from MusicBrainz / Cover Art Archive (CD-TEXT discs: cover, text gaps), cached in LittleFS | Working (exact disc ID match, cover on screen and in the web UI, offline from cache) |
| Display UI (album/artist/title, track, time, progress, state, volume, buffer) | Working |
| Diagnostics (I2S pin frequency check, test tone, slow/failed read logging) | Working |
| De-emphasis for pre-emphasis tracks | Working: 1st-order IIR, max. 0.07 dB from the analog curve. Tested with serial `E` (forced), no pre-emphasis disc at hand |
| Non-ASCII text on screen (CD-TEXT, online album info) | Working: Latin-1 + Latin Extended-A + typographic punctuation (generated fonts, `tools/font_conv.ps1`); other scripts show as `?` |
| Wi-Fi (station, mDNS `cdplayer.local`), firmware update over Wi-Fi (OTA) with rollback | Working: ~12 s per update (~120 KiB/s), slots alternate, new image confirms itself. Playback unaffected by Wi-Fi |
| Partition table: 2 × 2.5 MB OTA app slots + ~10.9 MB LittleFS at `/data` (for cached track lists / covers) | Working (album info cache in `/data/meta`). The app is ~1.6 MB |
| Power saving: drive spin-down after 2 min without playback; backlight dimming / off (web UI *Settings > Display*) | Working |
| Admin password for firmware upload and settings (HTTP Basic auth) | Working (optional, off by default) |
| Flash QIO mode | Prepared; needs one cable flash (OTA refuses a flash mode change) |

Verified with an **HP BD ROM BC-5501H** (slim SATA BD drive) behind an **Initio** USB bridge
(VID 0x13fd, PID 0x0840, interface subclass 0x02 / ATAPI, Bulk-Only), using a pressed audio CD with
CD-TEXT (Lorde, *Virgin*, 11 tracks). Also works with an old IDE drive, **JLMS XJ-HD165H**, behind a
**JMicron** USB-IDE bridge (VID 0x152d, PID 0x2338, subclass 0x06). This bridge needed exact READ TOC lengths (see the component README).

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

Normally: **build and update over Wi-Fi** (see [Firmware update over Wi-Fi](#firmware-update-over-wi-fi-ota)).
A cable flash is only needed the first time, for bootloader or partition table changes, or to recover a device
that can't reach Wi-Fi.

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

A cable flash writes the bootloader, the partition table, the app into `ota_0`, and the initial otadata (so
`ota_0` boots, whichever slot ran before). NVS (volume, Wi-Fi credentials) and the LittleFS partition are not
erased. `idf.py erase-flash` erases both.

### Wi-Fi setup access point

Without a usable Wi-Fi network, the player opens its own access point so the web UI can be reached for setup
([main/net.c](main/net.c), [main/captive_dns.c](main/captive_dns.c)):

- **When:** right at boot if no network is configured. If a network is configured but not reachable
  for 30 s (at boot, or after the connection was lost), the AP starts too. The station keeps retrying in
  the background (AP+STA mode). It can also be switched on or off by hand: *Setup AP on/off* in the web UI, or
  `api/cmd?c=setup_ap&v=1/0`. Switched on by hand, it stays on until switched off or the next reboot.
- **Name / password:** `CDPlayer-XXXX` (last MAC bytes), WPA2 with `CONFIG_CD_AP_PASSWORD` (menuconfig
  *CD player*, default `cdplayer`; shorter than 8 characters = open network). The display's top line shows
  the network name and password, alternating every 3 s with the address, while the AP is on and there is no
  other connection.
- **Web UI at http://192.168.4.1/.** A captive-portal DNS answers every name with 192.168.4.1 (it is offered
  via DHCP on the AP only; the device's own lookups still go through the station), and unknown URLs redirect
  to the UI, so phones open the page by themselves after joining. The page shows a setup banner and opens the
  settings; *Scan for networks* lists nearby networks to pick from.
- **After setup:** once the station has an IP address, the page shows the new address, and the AP turns off
  30 s later.
- AP and station share the radio, and each connection attempt scans all channels, which disturbs AP
  clients. While someone is connected to the AP, attempts are limited to one per minute (except right after
  new credentials were saved).
- **Security:** the web UI has no login and accepts firmware uploads, so set your own AP password.

### Firmware update over Wi-Fi (OTA)

One-time setup: set the Wi-Fi credentials with the serial command `W`. It prompts for the SSID and password
(stored in NVS; an empty SSID clears them). Example from the agent shell:
`python tools/sercmd.py COM23 20 "0.5:W" "1:MyNetwork~" "2:secret~" "15:w"`.
Alternatively, set defaults in menuconfig (*CD player → Default Wi-Fi SSID/password*). Stored NVS credentials
take precedence. `w` prints the connection state, IP, firmware version, OTA state, storage and heap.

```powershell
idf.py build
python tools/ota.py                    # default host cdplayer.local, image build/usbhost_cd.bin
python tools/ota.py 192.168.1.42       # by IP if mDNS doesn't resolve
```

The device accepts the upload at `POST /ota` and checks that it is an ESP32-S3 app image of this project.
It writes the image to the inactive slot (`ota_0`/`ota_1`), sequentially erasing as data arrives, and then reboots.
Playback is paused during the upload, and the screen shows `Firmware update NN%`. `tools/ota.py` then polls
`GET /info` (JSON: version, build time, ELF SHA-256, partition, OTA state, uptime) until the device reports
the uploaded image's ELF SHA-256. The same works with
`curl --data-binary @build/usbhost_cd.bin http://cdplayer.local/ota`.

**Rollback:** `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`. A freshly updated firmware boots as *pending-verify*.
It confirms itself once it has connected to Wi-Fi and the HTTP server is running again, i.e. once the next OTA
update is possible. If it crashes or never gets there, the **next reset** boots the previous firmware. Note:
nothing resets the board automatically when the new firmware hangs; press RST or power-cycle. If no Wi-Fi
credentials are configured, the firmware confirms itself immediately.

**Admin password** ([main/auth.c](main/auth.c)): optional, set in the web UI (*Settings → Admin password*, stored in
NVS) or as a default in menuconfig (`CONFIG_CD_ADMIN_PASSWORD`). When set, firmware upload, Wi-Fi settings, reboot,
the setup AP switch, deleting a cache entry and changing the password need it (HTTP Basic auth, any user name;
the browser asks). Player controls stay open. `tools/ota.py` takes it from the environment variable
`CDPLAYER_PASSWORD`. The serial command `P` clears it (recovery with physical access). Note that a refused upload
is only answered after the whole image was sent (no `Expect: 100-continue` in the ESP HTTP server).

Dependencies (managed components): `espressif/usb` (USB Host library, external since IDF 6.0),
`espressif/esp_lvgl_port`, `lvgl/lvgl` (v9), `espressif/mdns`, `joltwallet/littlefs`.

### Partition table ([partitions.csv](partitions.csv), 16 MB flash)

| Name | Type | Offset | Size | Use |
|---|---|---|---|---|
| nvs | data/nvs | 0x9000 | 24 KB | Settings (volume, Wi-Fi credentials). Same offset/size as the old table, so settings survived the switch |
| otadata | data/ota | 0xF000 | 8 KB | Which OTA slot boots, rollback state |
| phy_init | data/phy | 0x11000 | 4 KB | RF calibration data |
| ota_0 | app | 0x20000 | 2.5 MB | Firmware slot A |
| ota_1 | app | 0x2A0000 | 2.5 MB | Firmware slot B |
| storage | data/littlefs | 0x520000 | ~10.9 MB | LittleFS mounted at `/data` (`storage.c`). Formatted automatically on first mount. Meant for cached metadata from the internet (track lists, cover art) |

### Configuration (`sdkconfig.defaults`)

- `CONFIG_USB_HOST_HUBS_SUPPORTED=y`: a powered USB hub can be placed between the board and the drive.
- 16 MB flash, octal PSRAM at 80 MHz. `malloc()` puts allocations of 16 KB or more in PSRAM. USB transfer
  buffers are allocated by the USB library from internal DMA-capable RAM.
- Console on UART0, TX GPIO16 / RX GPIO21; no secondary console. The native USB port is the USB Host.
- LVGL: built-in Montserrat 14 (default font) and 28 (time, digits only); the text labels use the generated
  fonts in `main/fonts`. clib malloc/string/sprintf, so large LVGL allocations can go to PSRAM.
- `CONFIG_IDF_TARGET="esp32s3"`: so `sdkconfig` can be regenerated from the defaults (delete it and build).
- Custom partition table (`partitions.csv`), app rollback enabled in the bootloader.
- Wi-Fi: `CONFIG_ESP_PHY_ENABLE_USB=y` keeps the USB PHY powered when the Wi-Fi PHY initializes. Without it the
  drive on the native USB port would stop working. Wi-Fi/lwIP buffers may be allocated in PSRAM.
  `CONFIG_MDNS_ENABLE_CONSOLE_CLI=n` (no console component in the build).
- QIO flash at 80 MHz (`CONFIG_ESPTOOLPY_FLASHMODE_QIO`). OTA refuses images with a different flash mode
  (`ESP_ERR_OTA_SPI_MODE_MISMATCH`), so changing it needs a cable flash (bootloader + app).

## Application

| File | Role |
|---|---|
| [main/cd_player_main.c](main/cd_player_main.c) | Drive connect/disconnect, disc polling every 1 s (GESN media events, or TEST UNIT READY), tray lock + eject button, TOC/CD-TEXT loading, eject/load, serial commands |
| [main/player.c](main/player.c) | Playback engine: a reader task fills 12 × 16-sector chunks (~2.5 s, 450 KB, `NUM_CHUNKS`) in PSRAM; a writer task streams them to I2S in 2-sector slices, applies the volume and tracks the position and the buffered sector count. A generation counter drops stale chunks on seek/stop/unload. Plays continuously across tracks, skips data tracks, stops at the lead-out. Reads are retried 3×; slow (>500 ms) and failed reads are logged |
| [main/player.h](main/player.h) | Player API, status snapshot, volume cap (persisted in NVS by `player.c`) (`PLAYER_VOLUME_MAX`, `PLAYER_VOLUME_STEP`, `player_volume_display()`) |
| [main/audio_out.c](main/audio_out.c) | I2S0 master, 44.1 kHz, 16-bit stereo, Philips format, MCLK = 256·fs. DMA auto-clear outputs silence on pause or underrun. Pin check and test-tone diagnostics |
| [main/fonts](main/fonts) | Text fonts for the display (Montserrat 14/20 px with Latin-1, Latin Extended-A, punctuation and the LVGL symbols), generated by [tools/font_conv.ps1](tools/font_conv.ps1) |
| [main/ui.c](main/ui.c) | LVGL screen, refreshed every 200 ms from a `player_get_status()` snapshot (runs in the LVGL task) |
| [main/net.c](main/net.c) | Wi-Fi station: credentials from NVS (namespace `wifi`) or Kconfig, reconnect with backoff (1-30 s), no modem sleep, mDNS hostname `cdplayer` (`CONFIG_CD_HOSTNAME`), starts the HTTP server on the first IP address |
| [main/ota.c](main/ota.c) | HTTP server: `GET /info`, `POST /ota` (streams into the next OTA slot, validates, reboots); confirms a pending-verify firmware |
| [main/storage.c](main/storage.c) | LittleFS on the `storage` partition at `/data` |
| [main/auth.c](main/auth.c) | Admin password (HTTP Basic auth) for firmware upload and settings |
| [main/captive_dns.c](main/captive_dns.c) | DNS server for the setup AP: every A query answered with 192.168.4.1 |
| [main/meta.c](main/meta.c) | Album metadata: cache lookup, MusicBrainz disc ID query, Cover Art Archive download, JPEG decode + scale for the screen (see below) |
| [main/discid.c](main/discid.c) | MusicBrainz disc ID (SHA-1 + base64 over the TOC; own SHA-1, since mbedTLS 4 doesn't export it) |
| [main/web.c](main/web.c), [main/web/index.html](main/web/index.html) | Web UI: the page plus a JSON API (`/api/...`) on the OTA HTTP server |
| [components/tdisplay_s3](components/tdisplay_s3) | ST7789 i80 + LVGL port setup, ported from [hiruna/esp-idf-t-display-s3](https://github.com/hiruna/esp-idf-t-display-s3) (MIT) to IDF 6 |

On disc load and after each spin-up the drive is set to 4x (`SET CD SPEED`) for less noise; the log line `Read speed ...` shows the result and the speed the drive reports. CD audio needs 1x. `SET STREAMING` (the MMC-3 alternative) is implemented but off by default: the TSSTcorp SH-S182M took it as "full speed". For trying other drives: `api/cmd?c=speed&v=<kB/s, 176 = 1x, 0 = max>&m=<1 SET CD SPEED, 2 SET STREAMING, 3 both>`, applied at once, not saved.
A disc starts playing as soon as it is loaded, whether inserted or already in the drive at power-up.
This is the *Autoplay* setting in the web UI (NVS namespace `player`, key `autoplay`, default on).

### Album info and cover art

When a disc is loaded ([main/meta.c](main/meta.c)):

1. The MusicBrainz disc ID is computed from the TOC. A trailing data track (Enhanced CD) is excluded and the
   lead-out moved to its start - 11400, like libdiscid.
2. The cache is checked: `/data/meta/<discid>.json` (album, artist, track titles/artists, release ID) and
   `<discid>.jpg` (the cover as downloaded). A hit is applied right away, without a network connection.
3. Otherwise, as soon as Wi-Fi is connected: `GET https://musicbrainz.org/ws/2/discid/<id>?toc=...`
   (the TOC enables fuzzy matching if the exact ID isn't known). The chosen release is the one that lists this
   disc ID, has front cover art, and has the same track count. Then the cover:
   `https://coverartarchive.org/release/<mbid>/front-250` (or the release group's), redirected to archive.org.
   The result is cached.
4. The player shows the data wherever CD-TEXT has nothing (CD-TEXT always wins, so for CD-TEXT discs this
   mostly adds the cover). The cover is decoded
   (`espressif/esp_jpeg`, 1/2 scale), center-cropped and area-averaged to 72×72 for the screen.

Failures are retried after 5, 15, 60 and 180 s, then given up until the disc is reloaded. The reason of the last
failure is shown by `w` and in the web UI (Album info). A disc unknown to MusicBrainz is remembered until reboot, so it
isn't looked up again on every insert. Serial `M` or *Look up online again* in the web UI refreshes a cached
entry. While a lookup runs, a spinner turns in the cover area, on the display and in the web UI. The lookup typically
takes about 1 s for MusicBrainz and 5-20 s for the cover (archive.org is slow). It runs in its own task and
doesn't disturb playback.

**Export / import** (web UI, Album info): *Export all* downloads the whole cache as `cdplayer-meta.tar`
(`meta/<discid>.json` + `.jpg`), generated on the fly. That's about 1.2 MB for 38 discs and takes ~5 s. Windows
Explorer, 7-Zip and `tar` open it. *Import* takes such a tar: new entries are added, entries for the same disc
replaced, identical files left alone, and everything else in the cache kept. Only cache files are taken, from any
folder in the archive: `<28-character disc ID>.json` (a JSON object with that `discid`) and `.jpg` (a JPEG). Other
files are skipped, so an archive repacked from the extracted folder works too. Re-importing mostly identical data
takes ~6 s, but writing is slow (~30 KB/s: about 40 s for a full cache into an empty one); the web UI doesn't update
meanwhile. Playback isn't affected. Import needs the admin password if one is set.

Notes:
- **TLS:** the certificate bundle with `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_CROSS_SIGNED_VERIFY`. Without it,
  archive.org fails verification: its GoDaddy chain ends at the old Class 2 root, which is no longer in the
  bundle. mbedTLS and cJSON allocate in PSRAM. No SNTP: certificate validity dates aren't checked (IDF default).
- MusicBrainz asks clients for a User-Agent with contact info: optional `CONFIG_CD_META_CONTACT`
  (menuconfig *CD player*).
- The display fonts cover Latin-1, Latin Extended-A and typographic punctuation; other characters (e.g. Cyrillic,
  CJK) show as `?`. The web UI shows the original UTF-8.
- Progressive JPEGs can't be decoded by TJpgDec; such covers are skipped (not cached).

### Controls

The player is controlled through the web UI or serial commands. The onboard buttons are not used; dedicated
buttons will be added later. An earlier, uncommitted version used the two onboard buttons through the
`espressif/button` component (click / double click / hold events), which worked well for this.

### Web UI (http://cdplayer.local/)

Served by the player itself ([main/web/index.html](main/web/index.html), embedded in the firmware; [main/web.c](main/web.c)).
Works on phones. It refreshes once per second.

- **Now playing:** artist/album, title, track number, state, position, a progress bar (click to seek),
  and the transport buttons: previous, -10 s, play/pause, stop, +10 s, next, eject/load.
- **Volume** slider (as displayed, 0-100 %), buffer fill, underruns and read errors.
- **Track list** with CD-TEXT titles and lengths; click a track to play it.
- **Settings:** autoplay, Wi-Fi network (save and reconnect), firmware info, firmware upload (same as
  `tools/ota.py`; waits for the reboot and reports the running version), reboot, memory and storage use.

Player controls need no login; settings and firmware upload need the admin password if one is set (see above).
The JSON API can also be used by scripts:

| Request | Function |
|---|---|
| `GET /api/status` | Player state, position/length in seconds, volume (%), buffer, counters, CD-TEXT, Wi-Fi, firmware (version, ELF hash prefix, partition, OTA state, uptime), memory, storage |
| `GET /api/cover` | Cover art of the current disc (cached JPEG), 404 if none |
| `GET /api/meta/export` | Album info cache as a tar archive (see Album info) |
| `POST /api/meta/import` | Body: a tar archive (raw, e.g. `curl --data-binary @cdplayer-meta.tar`). Returns `{"imported", "unchanged", "skipped", "failed", "error"}`; HTTP 400 if it isn't a valid tar or is cut off (files before that point are kept) |
| `GET /api/tracks` | `[{"n", "audio", "len", "title", "performer"}, ...]` |
| `POST /api/cmd?c=<cmd>[&v=<n>]` | `play` (play/pause toggle), `stop`, `next`, `prev`, `skip&v=±s`, `seek&v=<s into track>`, `track&v=<n>`, `volume&v=<0-100>`, `eject`, `autoplay&v=0/1`, `meta` (look up album info again), `meta_forget` (delete the cache entry of the current disc), `setup_ap&v=1/0`, `speed&v=<kB/s>&m=<1-3>` (test, see above). Returns the status |
| `POST /api/wifi` | URL-encoded form `ssid=..&pass=..` (empty SSID clears) |
| `GET /api/wifi/scan` | Nearby networks `[{"ssid", "rssi", "open"}]`, strongest first (takes ~2 s) |
| `POST /api/reboot` | Restart |
| `POST /api/admin` | URL-encoded form `pass=..`: set the admin password (empty = remove) |

Example: `curl -X POST "http://cdplayer.local/api/cmd?c=next"`.

### Serial commands (COM23, 115200)

| Key | Action |
|---|---|
| `p` | Play / pause |
| `s` | Stop |
| `n` / `b` | Next / previous track |
| `f` / `r` | Seek +10 s / -10 s |
| `+` / `-` | Volume ±2 % as displayed (0-100 %, where 100 % = the internal cap; see below) |
| `e` | Eject / load |
| `<number>` Enter | Play track number |
| `i` | Print status |
| `a` | Diagnostics: measure MCLK/BCK/LCK frequencies and DIN activity on the I2S pins (pulse counter, outputs undisturbed) |
| `t` | Diagnostics: 1 kHz test tone for 3 s (pauses the player) |
| `w` | Network/firmware info: Wi-Fi state, IP, RSSI, firmware version/ELF SHA/partition/OTA state, storage use, free heap, album info state (disc ID, source, cover) |
| `M` | Look up album info / cover online again for the current disc (refreshes the cache) |
| `E` | Force the de-emphasis filter on all tracks on/off (to audition / test it; not saved) |
| `P` | Clear the web UI admin password |
| `W` | Set Wi-Fi credentials (prompts for SSID and password; Enter on an empty SSID clears them) |
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
(`PLAYER_VOLUME_STEP` = 1) is 2 % on the display. The volume is stored in NVS (namespace `player`, key `volume`,
2 s after the last change, so repeated steps cause a single flash write) and restored at boot, clamped to the current cap;
without a stored value it starts at the cap. The PCM5102 outputs about 2.1 V RMS at full
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
| Lorde - Virgin                    +--------+   |
| GRWM                    (scrolls) | cover  |   |
| Track 8 / 11                      | 72x72  |   |
|                                   +--------+   |
| 00:26               / 02:35                 > |
| [=========----------------------------------] |
| Vol 100%   Buf  92%   (wifi) (warn)     34:56 |
+------------------------------------------------+
```

**Backlight** (web UI *Settings > Display*, `POST /api/display`, stored in NVS namespace `display`):
dimming *only when idle* (default: nothing playing), *always* (also during playback) or *never*; brightness
when on (1-16, default 16) and dimmed (0-16, default 4); dim after 30 s without activity; off after 10 min
idle (0 = never, never during playback). Activity = a change of play state, track, volume, disc, status
message, firmware update or setup AP; saving the settings counts too. The backlight is an AW9364 LED
driver with 16 levels, set by pulses on GPIO38 (`tdisplay_s3_set_brightness()`).

The cover is only shown when there is one (album info section); the album and title lines then end left of it. The play state symbol sits at the right end of the time line. A Wi-Fi symbol after the buffer value means connected. During a firmware update, the title line shows the progress.

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

- **Hung drive:** the drive can stop answering completely. The USB bridge still enumerates and accepts
  commands, but never returns data or status. BOT reset recovery doesn't help, and neither does a USB bus reset:
  a root port power cycle and an ESP reboot were both tested, and both re-enumerate the bridge while the drive
  behind it stays hung. Only cutting the drive's power helps. Seen once with the HP BD drive (Initio bridge)
  after 18 read errors during playback. After two failed polls (~20 s), the screen shows *Drive not responding -
  power-cycle it*. The same message appears when a connected drive never answers INQUIRY. The tray button
  still works, because it doesn't depend on USB.

## Next steps

1. Physical buttons (transport + eject), with debouncing.
2. Final board: a **GPIO-controlled load switch on the drive's 5 V (VBUS)**, so a hung drive can be
   power-cycled automatically. On the T-Display S3, VBUS of the USB-C port isn't switchable.
3. Optional: a larger read-ahead buffer (`NUM_CHUNKS`; 10 s ≈ 1.8 MB of PSRAM) if longer read interruptions
   need to be bridged. The drive's own cache already covers several seconds.
