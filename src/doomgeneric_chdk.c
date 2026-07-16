// doomgeneric platform layer for the CHDK DOOM blob (approach B).
// All camera access goes through the callback table the launcher passes to doom_main.
#include "doomgeneric.h"
#include "doom_chdk_abi.h"
#include <stdint.h>

doom_cb_t *g_cb = 0;   // set by doom_main; used here and by chdk_syscalls.c

// direct on-screen debug marker (bypasses printf) - used to localize early crashes
void DBG(const char *s) { if (g_cb && g_cb->log) g_cb->log(s); }

// DOOM's live palette lives in i_video.c (global in CMAP256 builds): 256 entries, current
// gamma applied. We mirror the bitfield layout to read r/g/b and push it to the launcher
// whenever DOOM changes it (startup, damage/pickup tints). Launcher turns it into a
// DOOM-index -> nearest-solid-Canon-index LUT.
struct dcolor { unsigned b:8, g:8, r:8, a:8; };
extern struct dcolor colors[256];
extern int palette_changed;            // DOOM 'boolean' (int-sized)
static unsigned char s_pal[256 * 3];

uint32_t DG_GetTicksMs(void)           { return g_cb->get_ticks_ms(); }
void     DG_SleepMs(uint32_t ms)       { g_cb->sleep_ms(ms); }
void     DG_SetWindowTitle(const char *t) { (void)t; }
void     DG_Init(void)                 { }
void     DG_DrawFrame(void) {
    if (palette_changed) {
        int i;
        for (i = 0; i < 256; i++) {
            s_pal[i * 3 + 0] = colors[i].r;
            s_pal[i * 3 + 1] = colors[i].g;
            s_pal[i * 3 + 2] = colors[i].b;
        }
        if (g_cb->set_palette) g_cb->set_palette(s_pal);
        palette_changed = 0;
    }
    g_cb->draw_frame((const unsigned char *)DG_ScreenBuffer, DOOMGENERIC_RESX, DOOMGENERIC_RESY);
}
// --- save-name entry: a d-pad letter wheel (the camera has no keyboard) ----------------------
// DOOM won't save a game with an empty name, and its name field expects typed letters. While the
// engine is in save-name mode (saveStringEnter), remap the d-pad to drive a letter wheel:
//   UP/DOWN = cycle the current letter A..Z, RIGHT = lock it + start the next, LEFT = delete,
//   SET/DISP (ENTER) = save, MENU (ESC) = cancel.
// We feed DOOM synthetic key events (a letter, or BACKSPACE+letter to replace) via a tiny queue.
extern int saveStringEnter;            // m_menu.c global: nonzero while naming a save
static unsigned char kq[4];            // queued synthetic DOOM keycodes
static int kqn = 0, kqi = 0;
static char wheel = 'A';
static int  editing = 0;               // are we tuning a letter at the cursor?
static void kpush(unsigned char k) { if (kqn < (int)sizeof(kq)) kq[kqn++] = k; }

int DG_GetKey(int *pressed, unsigned char *key) {
    if (kqi < kqn) { *pressed = 1; *key = kq[kqi++]; if (kqi >= kqn) kqn = kqi = 0; return 1; }
    int p; unsigned char k;
    if (!g_cb->get_key(&p, &k)) return 0;
    if (saveStringEnter && p && (k == 0xad || k == 0xaf || k == 0xa1 || k == 0xa0)) {
        if (k == 0xaf) {                                   // DOWN: letter forward (A,B,C,D...)
            if (!editing) { wheel = 'A'; editing = 1; } else { wheel = (wheel == 'Z') ? 'A' : wheel + 1; kpush(0x7f); }
            kpush((unsigned char)wheel);
        } else if (k == 0xad) {                            // UP: letter back (Z,Y,X...)
            if (!editing) { wheel = 'Z'; editing = 1; } else { wheel = (wheel == 'A') ? 'Z' : wheel - 1; kpush(0x7f); }
            kpush((unsigned char)wheel);
        } else if (k == 0xa1) {                            // RIGHT: lock + start next letter
            wheel = 'A'; editing = 1; kpush((unsigned char)wheel);
        } else {                                           // LEFT (0xa0): delete last
            kpush(0x7f); editing = 0;
        }
        if (kqi < kqn) { *pressed = 1; *key = kq[kqi++]; if (kqi >= kqn) kqn = kqi = 0; return 1; }
        return 0;
    }
    if (saveStringEnter && p && (k == 13 || k == 27)) editing = 0;   // confirm/cancel ends editing
    *pressed = p; *key = k; return 1;
}

// Blob entry. Linker places this first (.text.entry) so it sits at the blob's load address.
__attribute__((section(".text.entry"), used))
extern char __bss_start[], __bss_end[];

// DOOM's "Quit Game" menu calls I_Quit(), which runs registered atexit handlers then returns
// (doomgeneric stubs out the real exit()). We register one that flags our loop to break, so the
// in-game Quit Game lands in the same clean teardown as the PRINT button.
static int wants_quit = 0;
static void on_doom_quit(void) { wants_quit = 1; }
extern void I_AtExit(void (*func)(void), int run_on_error);

int doom_main(doom_cb_t *cb) {
    char *p;                                       // zero BSS first, before touching any global
    for (p = __bss_start; p < __bss_end; p++) *p = 0;
    g_cb = cb;
    if (cb->magic != DOOM_ABI_MAGIC) return -1;   // ABI sanity
    cb->log("DOOM BLOB ENTERED");                  // first-boot proof: jump landed + callback works
    static char *argv[] = { "doom", "-iwad", "A/DOOM1.WAD", 0 };
    doomgeneric_Create(3, argv);
    I_AtExit(on_doom_quit, 1);                      // make in-game "Quit Game" exit cleanly
    for (;;) {
        doomgeneric_Tick();
        if (wants_quit || (cb->should_quit && cb->should_quit())) break;  // Quit Game or PRINT
    }
    cb->log("DOOM exit");
    return 0;
}
