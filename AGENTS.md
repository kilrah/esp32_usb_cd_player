This project started from Espressif's USB Host MSC example. The MSC driver has been replaced by our own ATAPI driver, now a separate component: [esp_usb_host_atapi](https://github.com/kilrah/esp_usb_host_atapi).
The goal of this project is to be able to read an audio CD using a drive connected to the USB Host port instead of a flash drive. The audio will then be fed into an i2s DAC and offer a user interface with a display and buttons.

Current state: a working CD player (I2S output to a PCM5102, LVGL UI on the T-Display S3, controlled by serial commands; physical buttons to come).
Wi-Fi with OTA firmware updates and a LittleFS partition for cached metadata have been added (see README).
See [README.md](README.md) for the status, hardware/pinout, controls and next steps, and
the [driver README](https://github.com/kilrah/esp_usb_host_atapi#readme) for the driver design.

## Working notes for agents

- **Build environment:** ESP-IDF v6.1 via EIM. In PowerShell, run `. C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1`
  first. `C:\esp\v6.1\esp-idf\export.ps1` is broken on this machine.
- **Boards / presets:** `idf.py build` = T-Display S3 (`default` preset: `build/`, `sdkconfig`), `idf.py --preset p4 build`
  = Guition JC1060P470-V2 (always write the -V2: there are three versions and the display only works on V2; ESP32-P4: `build_p4/`, `sdkconfig.p4`, `dependencies.lock.esp32p4`), `idf.py --preset s31 build`
  = ESP32-S31-Function-CoreBoard-1 (preview target; `build_s31/`, `sdkconfig.s31`, `dependencies.lock.esp32s31`; no
  display: `BOARD_HAS_DISPLAY 0` builds `main` without the UI and LVGL; ES8311 default, PCM5102 on GPIO25/23/21/35 as
  a Kconfig option; console on UART0 through the on-board USB-UART bridge, COM29; see the README). The S31's
  USB-UART has auto-reset, so the agent can reset it (pulse RTS with DTR low) and cable-flash it without the user.
  esptool has no stub for the S31 and its ROM can't `erase_flash`: blank regions with `write-flash` of 0xFF files
  (`nvs` at 0x9000, the first 64 KB of `storage` at 0x520000 is enough for LittleFS to reformat). Ethernet:
  boards with `BOARD_HAS_ETH` provide `board_eth_init()` (S31: YT8531 RGMII, Tx clock from the APLL; P4: RMII,
  IP101GR PHY, generic driver); `net.c` treats an Ethernet address like a station connection. Board code lives in
  `components/<board>` behind a common `board.h`; screen layouts are picked by resolution (`main/ui_layout.h`), not board.
  Common settings in `sdkconfig.defaults`, board ones in `sdkconfig.defaults.<target>`. P4 board details (console on
  its USB-Serial-JTAG port, COM13; all known pins/chips in `docs/JC1060P470-V2.md`; an RTS pulse didn't reset it, `api/reboot` does and the port survives the reboot; PCM5102 on GPIO1-4 by default, on-board mono ES8311 as a
  Kconfig option; Wi-Fi via the C6) are in the README. Every player defaults to `cdplayer.local`; the hostname
  is a web UI setting (`api/hostname`), so with two players online, use the IP or give one another name. Its DSI ISR must stay cache-safe (screen
  flashed during flash writes), which needs the IRAM flush callback in `jc1060p470_v2.c`.
- **Board: LilyGO T-Display S3** (non-touch). The optical drive is on the native USB port (USB Host). The PCM5102 is on
  GPIO43/44/18/17. The pinout is in the README; check it for conflicts before assigning new pins (the LCD uses many).
  - **Console:** external FTDI on **COM23** (board TX = GPIO16, RX = GPIO21). The FTDI has no DTR/RTS wiring, so the agent
    can't reset the board. Ask the user to press RST when the boot log is needed.
  - **Driving the app from the agent (preferred): the HTTP API**, so COM23 stays free for the user. `curl
    http://cdplayer.local/api/status` (or the IP; Python `requests`/`urllib` also work), `curl -X POST
    "http://cdplayer.local/api/cmd?c=next"`; see the README's Web UI section. Read the volume first, and never
    raise it in tests. `volume` is the displayed percent, and the user listens at a low level.
    Serial is still needed for logs (boot log, warnings) and for `a`/`t` diagnostics.
  - **Serial commands:** the firmware has single-key serial commands (`p s n b f r + - e i a t h`, `<number>` Enter).
    `a` measures the I2S pin frequencies, `t` plays a 1 kHz tone.
    Each command prints a `STATUS ...` line. This is the way to test playback without the user pressing buttons.
    Use `python tools/sercmd.py COM23 <seconds> "<at>:<keys>" ...` (`~` = Enter). Don't send a literal backslash-n:
    it gets executed as `\` (ignored) followed by `n` = next track. COM23 is often open in the user's own monitor;
    "Access denied" means you should ask them to close it.
  - **Flashing over Wi-Fi (preferred):** `idf.py build` then `python tools/ota.py` (host `cdplayer.local`, or pass the IP).
    It verifies that the new ELF SHA-256 is running. Needs Wi-Fi credentials on the device (`W` command, or check with `w`).
    No user action needed. Bootloader or partition table changes still need a cable flash.
  - **Flashing by cable:** only via native USB in download mode. Ask the user to unplug the drive, plug in the PC, and hold BOOT while pressing RST;
    the port appears as VID 0x303A PID 0x1001 (it was COM17). Then run `idf.py -p COM17 flash`. Poll for the port with
    `Get-CimInstance Win32_PnPEntity` instead of guessing the COM number. **After flashing, the board stayed in download mode**
    (COM17 still present) even though esptool reported a hard reset. The user has to press RST.
- **Serial capture:** `tools/sercmd.py` already does this right: DTR/RTS low **before** `open()`, and UTF-8 stdout,
  because the Windows console encoding crashes on log bytes. `idf.py monitor` is interactive and not usable from the agent shell.
- **Flash cycle cost:** a cable flash needs the user (unplug drive, BOOT+RST, RST afterwards, replug drive); OTA doesn't.
  Still build-check changes before uploading. Fixes that are built but not flashed should be mentioned to the user.
  A new OTA firmware that never reaches Wi-Fi is rolled back on the next reset (ask the user to press RST).
- **Drive power:** no power banks; they short the USB data lines for fast-charge signalling. If the drive enumerates and then
  drops off, or never shows up, suspect power first.
- **Test discs:** the user swaps discs, so check the TOC with `i` first. The reference disc is Lorde, *Virgin*
  (11 tracks, has CD-TEXT). Its first audio bytes at LBA 0 are `65 05 ef 02 aa 03 50 02 ...`, peak about -14 dBFS.
  Track 7 (1:57) is the shortest and is handy for testing the transition into the next track: `7~`, 11× `f`, `p`.
  A 13-track disc was also used.
- **Expected healthy playback:** `buf=` 90-97 % (~2.4 s), `underruns=0`, `read_errors=0`. The first read after
  spin-up takes 2-3 s and is logged as a slow read; that's normal.
- **Debugging USB link problems:** raise `CONFIG_LOG_MAXIMUM_LEVEL` to debug and call `esp_log_level_set()` for the tags
  `HUB`, `HCD DWC`, `ENUM`, `USBH`, `USB HOST`, `atapi_host`, `atapi_mmc`. Put the settings back afterwards.
- **Past root causes**, so they don't get re-investigated:
  - Drive enumerated, then dropped off the bus every ~3 s, even with no commands sent: VBUS brown-out through the
    old devkit's fuse. This was a hardware fix.
  - No console output on the T-Display S3: the silkscreen is misleading. The UART pins are GPIO16/21, not 1/2.
  - No audio from the PCM5102 even though all I2S signals were present (checked with `a`): the module's solder
    bridges were not set (XSMT must be H). Clipping at higher volume comes from the analog stage downstream;
    the volume is capped (`PLAYER_VOLUME_MAX` = 50, shown as 100 %).
  - Leftover white areas behind updated labels on the display: LVGL draw buffers in PSRAM, so the i80 DMA sometimes
    sent stale cache content. The buffers are now in internal DMA RAM (`buff_dma` in `tdisplay_s3.c`); keep them there.
  - Buffer gauge stayed full for several seconds when the disc was blocked, then emptied fast: the drive's own
    read-ahead cache keeps serving reads. Not a bug; the user is fine with the 2.5 s buffer.
  - READ TOC timed out (30 s, then BOT reset, forever) on a JMicron USB-IDE bridge (152d:2338): it can't handle
    an allocation length larger than the reply. READ TOC now reads the header first, then the exact length.
    Do the same for any new variable-length command. See the component README.
  - Eject didn't close an open tray (JLMS desktop drive): with the tray open it reports the generic sense 02/3A/00,
    not 3A/02, so polling reset the tray flag. The tray state now comes from GET EVENT STATUS NOTIFICATION
    when the sense is ambiguous. The HP laptop drive can't close its tray at all; that's normal.
  - Cover download failed with `esp-x509-crt-bundle: Failed to verify certificate` on archive.org: GoDaddy chain
    cross-signed by the old Class 2 root. Fixed with `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_CROSS_SIGNED_VERIFY`.
  - Intermittent `lookup failed`, `esp-tls: Failed to create socket` within ~15 ms: lwIP socket table exhausted.
    The web server can take max_open_sockets + 3, and with the default of 10 sockets, two browsers with the page
    open used them all. Fixed: `CONFIG_LWIP_MAX_SOCKETS=16` and `max_open_sockets = 5` (LRU purge) in `ota.c`.
    Keep this budget in mind when adding network clients.
  - "Drive error" forever, every bulk IN timing out, even after an ESP reboot or a USB root port power cycle
    (`usb_host_lib_set_root_port_power`, tried and removed): the drive itself was hung, behind a bridge that
    still enumerates. Only a power cycle of the drive helps (needs a VBUS switch on the final board). Don't
    retry USB-level resets for this. The app now shows "Drive not responding - power-cycle it".
  - Discs with a large CD-TEXT (41-track compilation): every audio read failed with `bulk_xfer: No memory for
    transfer`, then the drive wedged, although ~110 KB of internal RAM was free. The driver's USB buffer grew on demand
    and the heap fragmented around it. Fixed in driver v0.1.1: the 37 KB buffer is allocated once when the drive connects.
  - Every CSW timed out forever after an error: data toggle desync after CLEAR_FEATURE. This is fixed in
    `clear_halt()`; see the component README.
- **Test drives** (all behind the JMicron 152d:2338 USB-IDE bridge):
  - **JLMS XJ-HD165H** (2003): the main test drive. It honours SET CD SPEED. The disc slips a bit, which causes
    occasional read errors during spin-up and a slow recovery after a full spin-down. That's why the read-ahead
    is 8 s (`NUM_CHUNKS` = 40). Useful for testing imperfect conditions. Its first 2-3 TOC reads after "Disc
    ready" can be corrupt (a CSW at offset 80); the TOC check rejects them and the retry succeeds.
  - **TSSTcorp SH-S182M**: poor candidate, don't use it to judge changes. It reads audio at full speed whatever
    SET CD SPEED / SET STREAMING say (it reports 4x anyway), SET STREAMING made it worse (it no longer stayed slow
    when resuming from low idle), and it wedges on every ESP restart, even a clean one: INQUIRY times out and
    then the BOT reset on EP0 too. Only a power cycle of the drive helps. IDF can't halt/flush EP0, so such a
    transfer is never cancelled; the driver gives up after 2 s and marks the device lost (`xfer_lost`), and
    `free_device()` then leaks the device context on purpose, because the stack calls `transfer_cb()` for it
    later (freeing it caused a WDT reset on unplug).
- **The ATAPI driver lives in its own repo** (https://github.com/kilrah/esp_usb_host_atapi), pulled by `main/idf_component.yml` at a
  git tag (`kilrah/esp_usb_host_atapi`, not in the ESP Component Registry yet). It lands in
  `managed_components/kilrah__esp_usb_host_atapi`; the component manager's "doesn't match the directory name ."
  warning is harmless (git source at the repo root). To change the driver: clone the repo, temporarily replace the
  `git:`/`version:` lines with `override_path: <path to the clone>`, test here, commit and tag a new version there
  (and update its CHANGELOG), then point `version:` at the new tag. Never commit the `override_path`.
- Don't modify `managed_components/`: it is regenerated by the component manager. Work around library issues
  in our own components.
- **Setup AP testing:** `api/cmd?c=setup_ap&v=1` turns the AP on without dropping the station, so the device
  stays reachable on the LAN. Don't change the stored Wi-Fi credentials to test the "network unreachable"
  path: you don't know the user's password and can't restore it. Joining the AP from the PC's spare Wi-Fi
  adapter is risky: Windows asks all adapters' DNS servers, and the captive DNS answers every name with
  192.168.4.1. Ask the user to test with a phone instead.
- Internal RAM: ~108 KB free while playing, minimum ~64 KB during TLS lookups (mbedTLS, cJSON, Wi-Fi/lwIP
  buffers are in PSRAM; `malloc` ≥ 4 KB goes to PSRAM; Wi-Fi IRAM optimizations are off; the `cd_reader`,
  `cd_writer` and `usb_lib` stacks are in PSRAM). Keep big buffers in PSRAM; check with `m` (heap and every
  task's stack: location and unused bytes) after changes. A task stack may only be in PSRAM
  (`xTaskCreateWithCaps`) if the task never writes to flash (NVS, LittleFS, OTA). `m`'s "largest" block
  underreports: the 37.6 KB USB buffer was allocated fine while it showed 31.7 KB.
- Metadata test discs: Xandria, *Ravenheart* (no CD-TEXT, Enhanced CD with a data track 13) matches MusicBrainz by
  exact disc ID and has a cover. Cache files are in `/data/meta/`; `M` (or `api/cmd?c=meta`) forces a new lookup.
- The app is ~1.6 MB in a 2.5 MB OTA slot (`partitions.csv`). Don't move `nvs` (settings and Wi-Fi credentials live there).
- `sdkconfig` is generated: put settings in `sdkconfig.defaults` (target is pinned there), delete `sdkconfig` to regenerate.
- QIO flash mode is set (since the switch from DIO a cable flash was needed once; OTA refuses flash mode changes).
  Final button GPIOs are unknown.
