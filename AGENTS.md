This project started from Espressif's USB Host MSC example. The MSC driver has been replaced by the custom `components/usb_host_atapi` component.
The goal of this project is to be able to read an audio CD using a drive connected to the USB Host port instead of a flash drive. The audio will then be fed into an i2s DAC and offer a user interface with a display and buttons.

Current state: a working CD player (I2S output to a PCM5102, LVGL UI on the T-Display S3, controlled by serial commands; physical buttons to come).
See [README.md](README.md) for the status, hardware/pinout, controls and next steps, and
[components/usb_host_atapi/README.md](components/usb_host_atapi/README.md) for the driver design.

## Working notes for agents

- **Build environment:** ESP-IDF v6.1 via EIM. In PowerShell, run `. C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1`
  first. `C:\esp\v6.1\esp-idf\export.ps1` is broken on this machine.
- **Board: LilyGO T-Display S3** (non-touch). The optical drive is on the native USB port (USB Host). The PCM5102 is on
  GPIO43/44/18/17. The pinout is in the README; check it for conflicts before assigning new pins (the LCD uses many).
  - **Console:** external FTDI on **COM23** (board TX = GPIO16, RX = GPIO21). The FTDI has no DTR/RTS wiring, so the agent
    can't reset the board. Ask the user to press RST when the boot log is needed.
  - **Driving the app from the agent:** the firmware has single-key serial commands (`p s n b f r + - e i a t h`, `<number>` Enter).
    `a` measures the I2S pin frequencies, `t` plays a 1 kHz tone.
    Each command prints a `STATUS ...` line. This is the way to test playback without the user pressing buttons.
    Use `python tools/sercmd.py COM23 <seconds> "<at>:<keys>" ...` (`~` = Enter). Don't send a literal backslash-n:
    it gets executed as `\` (ignored) followed by `n` = next track. COM23 is often open in the user's own monitor;
    "Access denied" means you should ask them to close it.
  - **Flashing:** only via native USB in download mode. Ask the user to unplug the drive, plug in the PC, and hold BOOT while pressing RST;
    the port appears as VID 0x303A PID 0x1001 (it was COM17). Then run `idf.py -p COM17 flash`. Poll for the port with
    `Get-CimInstance Win32_PnPEntity` instead of guessing the COM number. **After flashing, the board stayed in download mode**
    (COM17 still present) even though esptool reported a hard reset. The user has to press RST.
- **Serial capture:** `tools/sercmd.py` already does this right: DTR/RTS low **before** `open()`, and UTF-8 stdout,
  because the Windows console encoding crashes on log bytes. `idf.py monitor` is interactive and not usable from the agent shell.
- **Flash cycle cost:** every flash needs the user (unplug drive, BOOT+RST, RST afterwards, replug drive). Batch changes
  and build-check them before asking for a flash. Fixes that are built but not flashed should be mentioned to the user.
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
  - Every CSW timed out forever after an error: data toggle desync after CLEAR_FEATURE. This is fixed in
    `clear_halt()`; see the component README.
- Don't modify `managed_components/`: it is regenerated by the component manager. Work around library issues
  in our own components.
- The app is ~650 KB in a 1 MB partition. Changing the partition table is deferred to the final board, but the app will outgrow it soon.
- Not yet done, deliberately (prototype board): partition table, QIO flash mode. Final button GPIOs are unknown.
