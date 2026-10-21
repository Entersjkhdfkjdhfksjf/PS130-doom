# Canon PowerShot SX130 IS (DIGIC 4, fw 101f) port

Why the SD1000 build aborts with `raw addr unexpected - abort` on an SX130 IS:

| | SD1000 (DIGIC 3) | SX130 IS 101f (DIGIC 4) |
|---|---|---|
| `hook_raw_image_addr()` | 0x10E706A0 | 0x41F3B020 |
| uncached alias bit | 0x10000000 | 0x40000000 |
| cached raw buffer base | 0x00E706A0 | 0x01F3B020 |
| raw buffer size | ~8 MB | 0x11CA240 (4080*3048*12/8, ~17.8 MB) |
| blob link/load address | 0x00E80000 | 0x01F40000 |
| PLAY viewport pointer | hardcoded `*(0x94408)` | `vid_get_viewport_active_buffer()` |

Changes: `src/doom_blob.ld` (link address), `src/gui_doom.c` (alias bit, BLOB_ADDR,
RAW_SIZE, viewport pointer, extra sanity checks and an on-screen `raw=` line).
The DOOM glue code (`doomgeneric_chdk.c`, `chdk_syscalls.c`, `doom_printf.c`) is unchanged.

Key map is unchanged: all the keys used exist on the SX130 IS. The Face button is
`KEY_PRINT` there (quit key).

## Build
* Blob: `XPACK=/path/to/xpack-arm-none-eabi-gcc-11.3.1-1.1 ./build-blob.sh`
* Launcher: copy `src/gui_doom.c` + `src/doom_chdk_abi.h` to `modules/games/` in a CHDK trunk
  checkout, copy `chdk-tree/makefile_usermodules.inc` to `modules/`, then
  `make PLATFORM=sx130is PLATFORMSUB=101f fir` (serial, not -j) and take `modules/.o/doom.flt`.

## SD card
`DOOM.BIN` + `DOOM1.WAD` in the card root, `doom.flt` in `CHDK/MODULES/`.
Boot in PLAYBACK mode, ALT mode, MENU > Games > DOOM.

Status: builds; NOT yet tested on hardware.
