# DOOM on the PowerShot SD1000 — port blueprint

Feasibility is CONFIRMED on-camera (see project memory). This is the build spine.

## What we're porting
- **doomgeneric** (id Software DOOM engine under the GNU GPL, via ozkl/doomgeneric).
  Platform-abstracted: we only implement 6 functions. Source lives at /tmp/doomgeneric
  (re-clone per session). Engine is GPL — our port stays GPL; ship source.
- **WAD**: shareware DOOM1.WAD only (id made the shareware episode freely distributable).
  Lives on the card, streamed lump-by-lump. ~4.2 MB.

## Hardware facts (proven)
- CPU: DIGIC III ARMv5TE ~100MHz, no FPU. DOOM is 16.16 fixed-point → no FPU needed.
- Arena: RAW image buffer, ~8 MB, address via `hook_raw_image_addr()` = 0x10e706a0
  (UNCACHED alias; phys ~14 MB in). Write-tested 8/8 MB clean in PLAY mode. This is
  DOOM's zone heap.
- Code heap: Canon system heap ~672 KB largest block — holds the engine code (module).
- Display: 8bpp palette overlay (`vid_get_bitmap_fb()`), ~360 wide. Use DOOM's CMAP256.
- Input: d-pad, SET, DISP, zoom lever, MENU. ~8 buttons.
- No audio.

## The 6 platform functions (doomgeneric_chdk.c)
- `DG_Init()` — claim the raw buffer, init our zone, set up the palette LUT.
- `DG_DrawFrame()` — blit DG_ScreenBuffer (320x200, 8-bit palette via CMAP256) to the
  bitmap overlay. Map DOOM's 256 palette → camera palette (set HW palette if possible,
  else a 256→nearest-solid LUT). Letterbox/scale 320x200 into 360x240.
- `DG_GetKey(pressed,key)` — translate camera buttons → DOOM keys (arrows=move/turn,
  SET=fire, DISP=use/open, zoom=strafe or weapon, MENU=esc). Edge-detect via kbd.
- `DG_GetTicksMs()` — `get_tick_count()`.
- `DG_SleepMs(ms)` — `msleep(ms)`.
- `DG_SetWindowTitle()` — no-op.

## PHASE 1 RESULT (proven Jun 27)
- Engine COMPILES for armv5te: 83/83 needed files clean, no source changes. (7 "fails" =
  allegro/sdl/win/linuxvt platform files we don't use.) Toolchain newlib headers used.
- Sizes: ~304 KB text, ~58 KB data, ~238 KB bss.
- TRUE external surface = 62 symbols: 6 are our DG_* funcs, 13 are __aeabi_* (libgcc auto),
  ~43 are standard libc that NEWLIB provides (stdio/malloc/string/printf/ctype).
- STRATEGY: link newlib libc.a + libgcc. We hand-write only ~14 funcs: the 6 DG_* +
  newlib syscall stubs (_sbrk → raw-buffer arena; _open/_read/_write/_close/_lseek →
  CHDK card API; _fstat/_isatty trivial). newlib does stdio/string/malloc on top.
- Module-size note CONFIRMED real: ~304KB text + 58 data + 238 bss ≈ 600KB must live in
  the ~672KB code heap. TIGHT. Fix: place DOOM bss + zone in the 8MB RAW BUFFER, keep only
  text+data (~362KB) in the heap → comfortable. May need a custom loader (load engine blob
  into malloc'd/raw memory) rather than a stock <100KB .flt module.

## PHASE 2 RESULT (proven Jun 27) — DOOM LINKS for the SD1000
- Wrote `doomgeneric_chdk.c` (6 DG_ stubs) + `chdk_syscalls.c` (newlib backend: _sbrk,
  _open/_read/_write/_close/_lseek, _fstat/_isatty/_kill/_getpid/_exit/_gettimeofday/_link/
  _unlink/mkdir). Linked all 85 .o + newlib(nano) + libgcc → **doom.elf, 0 undefined**.
- Linked sizes: **text ~312KB, data ~59KB, bss ~247KB** (~632KB total static).
- LOADER MATH (Phase 3): load text+data (~371KB) as the executable image into the ~672KB
  code heap (fits, ~300KB headroom). Put bss (~247KB) + the zone heap in the 8MB RAW BUFFER.
- BUILD: zsh does NOT word-split unquoted vars — inline gcc flags (don't use $CF). Flags:
  -march=armv5te -mtune=arm946e-s -Os -fno-strict-aliasing -fcommon -DNORMALUNIX -DCMAP256
  -DDOOMGENERIC_RESX=320 -DDOOMGENERIC_RESY=200. Link: --specs=nano.specs -lm. Exclude
  platform files: doomgeneric_{xlib,sdl,win,linuxvt,allegro,...}, i_allegro*, i_sdl*.

## The libc shim (SMALL — newlib does the heavy lifting) — chdk_syscalls.c
DOOM needs, CHDK modules don't fully provide:
- file IO: fopen/fread/fseek/ftell/fclose/fwrite + raw open/read/lseek/close → back with
  CHDK's card file API (open/read/lseek/close are exported). mmap (6 uses, WAD mapping) →
  replace with malloc+read (or a fake mmap over a read buffer).
- malloc/free/calloc/realloc → our raw-buffer zone allocator (bump/free-list over 8 MB).
- string.h (memcpy/memset/strcmp/strcpy/strlen/...) → CHDK exports many; fill gaps.
- ctype.h (toupper/isdigit/...) → trivial inline.
- math.h (5 files) → DOOM core is fixed-point; the math.h uses are minor (abs, a few). Stub
  or provide; avoid pulling in float libm.
- printf/vsnprintf/stdarg → route to CHDK console or stub.
- errno/assert → minimal.

## Phases (compile-iterate marathon)
1. **Compile** the engine for ARM as a CHDK module: stand up chdk_libc shim until all 95
   files build. Milestone = it links into a .flt. (Biggest risk: module load size ~300KB+.)
2. **Memory**: malloc → raw-buffer zone. Boot DOOM far enough to allocate.
3. **Display**: CMAP256 + DG_DrawFrame → overlay. First pixels on screen.
4. **Input + timing**: playable controls.
5. **WAD from card**: fopen shim reads DOOM1.WAD. Title screen → E1M1.
6. **Tune**: palette, controls, framerate, letterbox.

## PHASE 3 STATUS (in progress Jun 27) — the loader, the real wall
- Relocatable module link WORKS: DOOM objs + newlib(arm/v5te/softfp) libc/libm/libgcc,
  linked with CHDK LDOPTS (link-boot.ld, -r, -Ttext 0x24) → doom_mod.elf, 719KB. Clean.
- **WALL: `elf2flt` chokes** → "unknown segment 492 for name __aeabi_d2ulz". CHDK's elf2flt
  can't map some newlib/libgcc sections (double->u64 conv, pulled in by newlib printf %f
  float support). Approach A (DOOM as one big .flt) fights elf2flt's limits; likely
  whack-a-mole across more libgcc/newlib sections even if this one's killed.
- TWO PATHS:
  - A) Strip float/double usage (avoid printf %f, etc.) so the problem libgcc routines
    aren't pulled, and/or patch elf2flt to map the extra sections. Fragile.
  - B) **(recommended) Skip elf2flt. Build DOOM as a standalone flat/PIC binary with the
    normal linker, ship it as a data file on the card. Write a TINY normal CHDK .flt
    "launcher" that: allocs memory (heap for code, raw buffer for bss+zone), reads the DOOM
    blob from the card, applies relocations (or build -fPIC), and jumps to its entry via a
    function-pointer table (DG_/syscalls call back into CHDK through the table).** Sidesteps
    elf2flt entirely; standard way to load a big payload on these cams.
- DECISION: went with approach B.

## PHASE 3 — APPROACH B BLOB DONE (Jun 27): DOOM.BIN builds (no elf2flt)
- ABI: `src/doom_chdk_abi.h` (doom_cb_t = function-pointer table: timing/input/draw/
  set_palette/file IO/arena/log). Launcher fills it, passes to doom_main(cb).
- Glue: `src/doomgeneric_chdk.c` (DG_* -> g_cb->*, plus doom_main entry in .text.entry),
  `src/chdk_syscalls.c` (_sbrk over g_cb->arena; _open/_read/... -> g_cb->f_*; fd offset+3
  so 0/1/2 stay console; _init/_fini stubs).
- Link: `src/doom_blob.ld` flat image at **0x00E80000** (inside raw buffer, CACHED), no crt0
  (-nostartfiles), newlib(arm/v5te/softfp) + libgcc. objcopy -O binary -> **DOOM.BIN 481KB**.
- Symbols: doom_main @ 0x00E80000 (entry); bss 0x00EF5970..0x00F31768 (~241KB);
  _blob_end 0x00F31768. Launcher: load 481KB to 0x00E80000, zero [bss_start,bss_end), set
  arena = _blob_end .. (raw_base+8MB) ~7.4MB for the zone, flush caches, jump 0x00E80000.
- Reproduce: `chdk-study/doom/build-blob.sh` (re-clones engine, drops in src/, builds).
- NEXT (Phase 3 finish): the LAUNCHER .flt — alloc/locate raw buffer, read DOOM.BIN from
  card to 0x00E80000, zero bss, build doom_cb_t (get_tick_count/msleep/kbd/CHDK file IO/raw
  addr), CLEAN D-cache + INVALIDATE I-cache for the code region (critical for exec of freshly
  written code; find CHDK cache ops), then ((int(*)(doom_cb_t*))0x00E80000)(&cb). First boot
  = first on-camera test. Then Phase 4 (draw_frame->overlay) gives pixels.

## PHASE 3b — LAUNCHER BUILT (Jun 27), awaiting first boot
- `src/gui_doom.c` = CHDK launcher module (tiny .flt, 1692B, builds via elf2flt fine since
  it's small + uses CHDK libc). PLAY mode. Reads A/DOOM.BIN -> 0x10E80000 (uncached), reads
  header word0=entry word1=blob_end, fills doom_cb_t (ticks/sleep/getkey(stub)/draw(basic
  8bpp blit, palette not yet mapped)/file IO via CHDK open/read/.../arena=blob_end..raw+8MB),
  dcache_clean_all(), jumps entry(&cb). On-screen log shows progress + routes DOOM printf.
- Blob header: word0 entry, word1 _blob_end (LONG() in doom_blob.ld) → launcher is
  rebuild-proof (reads addrs from blob, not hardcoded). bss self-zeroed by doom_main.
- DEPLOYED: doom.flt -> CHDK/MODULES, DOOM.BIN -> card root. **DOOM1.WAD (shareware, freely
  distributable) must be placed at card root by Dane** to load past the title; without it,
  first boot still proves execution (DOOM prints IWAD-not-found via our log).
- FIRST BOOT (pending, needs camera): ALT>Games>DOOM in PLAY mode. Win = see "DOOM BLOB
  ENTERED" + DOOM startup log. Crash/black = jump/cache/addr issue (battery pull, non-destruct).
- KNOWN-NEXT after boot: uncached exec is SLOW → Phase: switch to cached + figure I-cache
  invalidation (icache_flush_all not module-exported; call firmware stub by addr, or other).
  Phase 4: real palette mapping (set_palette + DOOM 256 pal -> overlay) for correct colors.

## Open risks
- Module loader handling a ~300KB+ .flt (modules are normally <100KB). May need to load
  the engine as a payload into malloc'd memory instead of a standard module, or split.
- Palette: can we reprogram the HW palette for true color, or are we stuck mapping to ~16
  solids? Affects looks, not playability.
- Using the raw buffer must stay PLAY-mode only (don't shoot mid-game).

---

## RESULT (2026-06-27): IT RUNS. Playable + snappy on the SD1000.

What shipped and what mattered:
- **Standalone blob + tiny launcher** (approach B) was the right call. elf2flt couldn't handle
  full newlib; a flat binary loaded by a ~2KB CHDK module sidestepped all of it.
- **Bump allocator over the raw buffer**, not newlib malloc (newlib's dlmalloc crashed on the
  6MB zone request bare-metal). free() is a no-op; DOOM's zone manages itself.
- **Own printf family** (doom_printf.c): newlib's vfprintf hangs here on %d/%x; plain strings
  print fine. Wrote a compact formatter w/ width/precision.
- **WAD gotcha:** a non-standard DOOM1.WAD missing the STBAR lump fataled ST_Init. Standard
  shareware WAD fixed it. Not our bug.
- **THE SPEED WIN = run CACHED.** First boot ran from the UNCACHED alias (0x10E80000) for
  safety = slideshow. Relinking the blob to the CACHED alias (0x00E80000) and executing there
  made it snappy (~several x). Requirements:
  - Link blob at 0x00E80000; all absolute addrs (fn ptrs, header words) must match the exec alias.
  - Launcher: strip the 0x10000000 alias bit off hook_raw_image_addr() so load+arena math is cached.
  - Before jump: dcache_clean_all() (exported) THEN invalidate I-cache.
  - **icache_flush_all is NOT exported to modules** (only dcache_clean_all is). Inline the ARMv5
    CP15 op yourself: `mcr p15,0,r0,c7,c5,0`. Modules build THUMB and MCR is ARM-only on ARMv5,
    so the fn must be `__attribute__((naked, target("arm")))` with its own `bx lr`.
- **Color: still the open item.** Mapping DOOM's 256 colors -> ~14 confirmed-solid Canon indices
  (nearest RGB) kills most dither but detailed/gradient scenes (title screen) look "fried"; flat
  in-game walls look fine. Tried decoding the live HW palette (PALDUMP.BIN, 0x561c0): first 16
  entries are AYUV-ish and entries decode partially (idx2->red, idx12->blue) but the 8bpp-byte
  -> entry mapping is a HW function we didn't fully reverse. Parked; it's looks, not playability.
- Input: poll kbd_is_key_pressed in the blob's loop (Canon's kbd task keeps physical state live
  even while we never return to the GUI task). Emit one DOOM key transition per get_key call,
  round-robin so no key starves.
- Known-good build saved in doom/known-good/ (verified-working fallback).
