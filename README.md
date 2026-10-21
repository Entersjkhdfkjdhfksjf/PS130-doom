# DOOM on a Canon PowerShot SX130 IS

DOOM running on the Canon PowerShot SX130 IS (firmware 101f) via [CHDK](https://chdk.fandom.com/wiki/CHDK).
It loads from the SD card. Canon's firmware is never modified and nothing is flashed or soldered.

This is a port of [DaneBentley/sd1000-doom](https://github.com/DaneBentley/sd1000-doom)
(DOOM on the SD1000 / IXUS 70) to the SX130 IS. The engine is
[doomgeneric](https://github.com/ozkl/doomgeneric).

**Tested on:** SX130 IS, firmware 101f, CHDK 1.6.1 (revision 6315).
Other SX130 IS firmware versions (101c, 101d) are untested and would need their own build.

## Install (prebuilt)

1. Use a CHDK-bootable SD card for the SX130 IS with CHDK 1.6.x for `sx130is` firmware `101f`
   ([how to prepare the card](https://chdk.fandom.com/wiki/Prepare_your_SD_card)).
2. Copy `prebuilt-sx130is-101f/DOOM.BIN` to the **root** of the card.
3. Copy `prebuilt-sx130is-101f/doom.flt` to `CHDK/MODULES/` on the card.
4. Put the standard shareware `DOOM1.WAD` in the **root** of the card
   ([where to get it](https://doomwiki.org/wiki/DOOM1.WAD)). Repacked WADs may crash on startup.
5. Boot the camera in **PLAYBACK mode** (the memory DOOM uses is only free when you are not shooting).
6. Enter CHDK ALT mode, then open **MENU > Games > DOOM**.

## Controls

| Button | Action |
|---|---|
| D-pad up | Move forward |
| D-pad down | Move backward |
| D-pad left | Strafe left |
| D-pad right | Strafe right |
| Zoom lever (tele / zoom in) | Turn right |
| Zoom lever (wide / zoom out) | Turn left |
| Shutter (full press) | Fire |
| Shutter (half press) | Run |
| SET | Use / open doors (in game), confirm (in menus) |
| MENU | DOOM menu / Esc |
| DISP | Enter (confirm) |
| Face button | Quit back to CHDK |

## Not working / limits

* No sound.
* Shareware `DOOM1.WAD` only has been tested.

## What changed from the SD1000 version

The SD1000 build checks that the camera's raw image buffer is at an SD1000 address and aborts with
`raw addr unexpected - abort` on anything else. The SX130 IS has a different chip (DIGIC 4), so:

| | SD1000 (DIGIC 3) | SX130 IS 101f (DIGIC 4) |
|---|---|---|
| `hook_raw_image_addr()` | 0x10E706A0 | 0x41F3B020 |
| uncached alias bit | 0x10000000 | 0x40000000 |
| cached raw buffer base | 0x00E706A0 | 0x01F3B020 |
| raw buffer size | ~8 MB | 0x11CA240 (~17.8 MB) |
| blob link / load address | 0x00E80000 | 0x01F40000 |
| playback viewport pointer | hardcoded address | `vid_get_viewport_active_buffer()` |

Details are in [docs/SX130IS-PORT.md](docs/SX130IS-PORT.md).
The original SD1000 prebuilt files are still in `prebuilt/` and work only on that camera.

## Build it yourself

Toolchain: [xPack arm-none-eabi-gcc 11.3.1](https://github.com/xpack-dev-tools/arm-none-eabi-gcc-xpack/releases/tag/v11.3.1-1.1).

**The blob (`DOOM.BIN`):** `XPACK=/path/to/xpack-arm-none-eabi-gcc-11.3.1-1.1 ./build-blob.sh`
It clones doomgeneric, adds the glue from `src/`, and links a flat binary at `0x01F40000`
using `src/doom_blob.ld`. The result is written to `DOOM.BIN` in the repo root.

**The launcher (`doom.flt`):** build inside a CHDK trunk checkout.

1. Copy `src/gui_doom.c` and `src/doom_chdk_abi.h` to `modules/games/`.
2. Copy `chdk-tree/makefile_usermodules.inc` to `modules/`.
3. Put the ARM toolchain on your `PATH` and run
   `make PLATFORM=sx130is PLATFORMSUB=101f fir` (serial, not `-j`).
   The full build may stop at the end on a missing `bin/` directory, but the module is already built.
4. Take `modules/.o/doom.flt`.

`BLOB_ADDR` in `src/gui_doom.c` and the address in `src/doom_blob.ld` must match.

## Credits and license

* [DaneBentley/sd1000-doom](https://github.com/DaneBentley/sd1000-doom): the original CHDK launcher + blob design.
* [ozkl/doomgeneric](https://github.com/ozkl/doomgeneric) and id Software's DOOM source release.
* [CHDK](https://chdk.fandom.com/wiki/CHDK).

GPL-2.0, same as doomgeneric and the original DOOM source. The prebuilt `DOOM.BIN` contains compiled
doomgeneric; the complete corresponding source is this repo plus doomgeneric.
`DOOM1.WAD` is not included; get the shareware WAD from an official mirror.
