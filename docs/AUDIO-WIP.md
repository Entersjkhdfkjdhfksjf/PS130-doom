# SD1000 (102a) audio reverse-engineering — for real DOOM sound

Goal: stream DOOM's mixed PCM to the speaker. Strategy mirrors the video hijack: find where the
audio hardware reads its samples, feed it ours.

## Firmware
- ROM dumped from the camera via the `romdump` module -> `chdk-study/doom/ROM.BIN` (4 MB, base **0xff810000**).
- Disassemble: `arm-none-eabi-objdump -D -b binary -m arm --adjust-vma=0xff810000 ROM.BIN`.
- Re-dump anytime with the romdump module (read-only, safe).

## Audio hardware (identified)
- **Codec: Wolfson WM1400** (string `AudioIC_WM1400.c`). Driver: `SoundDevice_CODEC.c`.
- **ASIF (Audio Serial Interface) DMA engine @ MMIO base `0xC0920000`** (string `ASIF.c`).
  Registers seen used: +0x00, +0x04, +0x08, +0x0C, +0x10, +0x204, +0x208, +0x238, +0x320.
- Path: ASIF DMA reads a PCM buffer in RAM -> WM1400 -> speaker. DMA-complete fires an IRQ
  (handler IDs 0x2c/0x2d/0x42/0x43 registered by the init fn) -> refill next buffer = streaming.

## Key firmware addresses
- `StartSoundPlay_FW`  0xffa67a38  (wrapper; plays a pre-configured sound from struct @ RAM 0x9cfc)
- `InitializeSoundRec_FW` 0xffa67a60 / `StartSoundRecord_FW` 0xffa677dc / `FreeBufferForSoundRec_FW` 0xffa676a4
- `task_SoundPlay` 0xffb0d6e4 (msg-loop; state vars @ RAM 0x98a30..0x98b30, flags 0xd120/0xd124)
- `task_WavRead` 0xffb0e0c4 / `task_WavWrite` 0xffb0eda0
- `BeepDrive_FW` 0xffa2df50, `task_BeepTask` 0xffa2da50 (piezo beeper — PC-speaker fallback path)
- `PT_PlaySound` 0xffb0b2ec (canned beeps; CHDK `play_sound()` wraps this — safe, known)

## ASIF.c code region (0xffa33xxx) — function entry points
- 0xffa336c8  push {r4-r10,lr}  -> **ASIF init**: programs 0xC0920000 regs, registers DMA IRQ handlers.
- 0xffa33988  push {r4-r11,lr}  -> large fn, candidate for **StartASIFDMADAC** (buffer+size+callback).
- 0xffa33354, 0xffa33564, 0xffa335e8, 0xffa3363c -> smaller ASIF helpers.
- TODO: confirm which fn = StartASIFDMADAC / SetNextASIFDACBuffer (args: buf ptr, sample count,
  completion callback). That's the clean call to hand ASIF our PCM.

## Implementation plan
1. DOOM side: compile with FEATURE_SOUND + a custom `DG_sound_module` that mixes the 8 sfx channels
   into a PCM ring buffer (port the mix loop from i_sdlsound.c, drop SDL). Output ~11025 Hz, signed.
   Music (MUS/MIDI) = skip for now; sfx only.
2. Camera side: from the launcher, either
   (a) call Canon `StartASIFDMADAC`/`SetNextASIFDACBuffer` with our buffer + a refill callback, or
   (b) program ASIF @ 0xC0920000 + WM1400 directly (more control, more risk).
   Double-buffer: on DMA-complete IRQ/callback, swap to the freshly-mixed half.
3. Add an `audio` callback set to the doom_cb_t ABI so the blob hands PCM to the launcher.
4. Sync DOOM's mix rate to the ASIF playback rate; keep the buffer fed each tick.

## Risks
- Driving ASIF/codec wrong -> no sound or a hang (battery-pull recoverable, not a brick).
- Need the codec powered/unmuted in PLAY mode (it may be off when not playing video/voice memo).
- Real-time refill timing vs DOOM's ~12 fps display loop (audio must not underrun).

## Session 2 findings (the codec-power saga)
- **The ASIF DMA WORKS.** `StartASIFDMADAC(buf0,n0,buf1,n1,cb,0)` @ 0xffa33988 returns cleanly and
  fires the completion callback once per buffer (confirmed cb=2 on camera). Double-buffered streaming
  is real. Buffers must be UNCACHED (use the raw-image-buffer alias, hook_raw_image_addr()).
- **But no sound: the WM1400 codec is powered OFF when we run.** Canon powers it only transiently,
  during real sound playback. There is NO clean standalone "power on codec" call - the codec config
  fns (0xffa32e1c etc.) are entangled with sound-subsystem globals (0x9cfc, 0x565e0, 0x8abc, 0x70f38).
- **play_sound() = the PIEZO buzzer (BeepDrive path), NOT the WM1400 speaker.** Dead end for codec power.
  Also: it was silent from a module even with mute off (the UI beep path differs).
- **Clean API found: Canon event procedures.** `PLAYSOUNDDATA` / `RECORDSOUNDDATA` are registered procs.
  `StartSoundPlay_FW` (0xffa67a38) runs PLAYSOUNDDATA on a sound described by globals:
  buffer ptr @ **0x9cfc**, size @ **0x70f38** (worker 0xffa67850 copies them to 0x9d00/0x9d04).
  CHDK can call procs via `_ExecuteEventProcedure(name, ...)` (stub 0xff814a58) + call_func_ptr.
- **TESTED: set globals + StartSoundPlay() -> returns ret=1 (abort), no sound, no crash.** ret=1 means
  the PLAYSOUNDDATA proc returned not-ready: the **sound controller/play system isn't initialized** in
  our PLAY-mode module context (proc not live / device not open).
- **NEXT LAYER:** find + run the sound-play system init (registers PLAYSOUNDDATA, creates the SoundPlay
  task, opens the device). Candidates: "SystemEventInit" event proc (CHDK uses it), SoundPlayController.c
  / SoundPlayer.c init, or InitializeSoundRec's play analog. Then retry StartSoundPlay.
- Status: hardware proven, software stack is N layers of init deep; each layer = one camera round-trip.
  This is now a multi-session research project, not a quick win.
