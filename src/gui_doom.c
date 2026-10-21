// DOOM launcher - CHDK module for the Canon PowerShot SX130 IS, fw 101f (PLAY mode).
// (Ported from the SD1000/IXUS 70 launcher: see docs/SX130IS-PORT.md for the per-camera changes.)
// Loads the standalone DOOM blob (A/DOOM.BIN) into the raw buffer's uncached alias, wires a
// callback table (timing/input/draw/file IO/arena), and jumps into it. The blob runs uncached
// (no I-cache invalidation needed). DOOM reads A/DOOM1.WAD (shareware) via the file callbacks.
//
// First boot = proof of execution: you should see "DOOM BLOB ENTERED" then DOOM's own startup
// log (or its IWAD-not-found error if DOOM1.WAD isn't on the card). Power-cycle to exit.
#include "camera_info.h"
#include "keyboard.h"
#include "lang.h"
#include "conf.h"
#include "gui.h"
#include "gui_draw.h"
#include "gui_lang.h"
#include "gui_mbox.h"
#include "modes.h"
#include "clock.h"
#include "stdlib.h"
#include "module_def.h"
#include "doom_chdk_abi.h"

extern void *hook_raw_image_addr(void);
extern void *vid_get_viewport_active_buffer(void);   // in PLAY mode = Canon's display viewport (fb_d)
extern int   vid_get_viewport_byte_width(void);
extern void  dcache_clean_all(void);
extern unsigned char *vid_get_bitmap_fb(void);
extern void  vid_bitmap_refresh(void);     // repaint Canon's bitmap/overlay (restore screen on exit)
extern void  gui_set_need_restore(void);   // tell CHDK its OSD needs a full redraw
extern void  exit_alt(void);               // leave ALT mode -> unload module + back to clean Playback

void gui_game_menu_kbd_process();
int  gui_doom_kbd_process();
void gui_doom_draw();

// The MENU button is DOOM's own menu (we poll it as ESC). The framework's default game "menu
// button" handler would instead EXIT the module + drop out of ALT mode the instant MENU is
// pressed - which un-masks the keys to Canon (photo browser) and exposes the CHDK games menu
// behind DOOM (so SET then launches Doodle/Flappy). Override it to a no-op so MENU stays DOOM's.
// We exit DOOM only via the PRINT button (handled explicitly after the blob returns).
static void gui_doom_menu_btn(void) { }

gui_handler GUI_MODE_DOOM =
    { GUI_MODE_MODULE, gui_doom_draw, gui_doom_kbd_process, gui_doom_menu_btn, 0, GUI_MODE_FLAG_NODRAWRESTORE };

// SX130 IS (DIGIC 4): hook_raw_image_addr() = 0x41F3B020 ("CRAW BUFF" in Canon's log). DIGIC 4
// marks the UNCACHED alias with bit 0x40000000 (DIGIC 3 / SD1000 used 0x10000000), so the cached
// alias is 0x01F3B020. The raw buffer is 4080*3048*12/8 = 0x11CA240 bytes (~17.8MB).
// The blob is linked at BLOB_ADDR, the first 64KB boundary above the buffer base. KEEP THIS IN
// SYNC with src/doom_blob.ld.
#define UNCACHED_BIT 0x40000000u
#define BLOB_ADDR 0x01F40000u         // CACHED alias, inside the raw buffer
#define RAW_SIZE  0x011CA240u         // CAM_RAW_ROWPIX*CAM_RAW_ROWS*12/8

// Invalidate the entire instruction cache (ARMv5 CP15 c7,c5,0). The module runs privileged, so we
// issue this directly (icache_flush_all isn't exported to modules; only dcache_clean_all is). MCR
// is ARM-only on ARMv5, but modules build as Thumb, so force this one function to ARM and make it
// naked with its own BX LR. Must run after dcache_clean_all and before jumping into new code.
static void __attribute__((naked, target("arm"))) icache_invalidate_all(void) {
    asm volatile (
        "mov r0, #0\n\t"
        "mcr p15, 0, r0, c7, c5, 0\n\t"
        "bx  lr\n\t"
    );
}

static int launched = 0;
static int logy = 4;
static doom_cb_t cb;                  // must outlive the call (we never return)

// --- on-screen log (DOOM printf + our markers land here) ---------------------
static void scr_log(const char *s) {
    if (logy > camera_screen.height - 16) {       // wrap: clear + restart at top
        draw_rectangle(camera_screen.disp_left, 0, camera_screen.disp_left + camera_screen.disp_width - 1,
                       camera_screen.height - 1, MAKE_COLOR(COLOR_BLACK, COLOR_BLACK), RECT_BORDER0 | DRAW_FILLED);
        logy = 4;
    }
    draw_string_scaled(camera_screen.disp_left + 4, logy, s, MAKE_COLOR(COLOR_BLACK, COLOR_WHITE), 1, 1);
    logy += 16;
}

// --- callbacks the blob calls -----------------------------------------------
static unsigned int cb_ticks(void)            { return (unsigned int)get_tick_count(); }
static void         cb_sleep(unsigned int ms) { msleep(ms); }

// Color: render DOOM into the camera's VIDEO/viewport layer (real YUV), NOT the 8bpp UI overlay.
// The UI overlay only has ~14 solid colors + dither = "fried". The video layer is full-color
// (it's what makes photos/videos look good). SD1000 PLAY viewport: 720x240, byte_width 1080,
// format UYVYYY = 6 bytes per 4 px: [U, Y0, V, Y1, Y2, Y3], U/V signed (0 = neutral).
// We precompute each DOOM palette index -> (Y,U,V) on palette change, plus nearest-neighbour
// scale maps so DOOM's 320x200 fills the whole 720x240 viewport (full screen, no black bars).
#define VP_BW   1080          // UYVYYY row stride; also checked at runtime against vid_get_viewport_byte_width()
#define VP_W    720           // pixels per row
#define VP_H    240           // rows in PLAY mode
#define VP_FB_D ((unsigned char *)vid_get_viewport_active_buffer())   // PLAY display buffer

static unsigned char g_y[256];           // luma per index (0..255)
static signed char   g_u[256], g_v[256]; // chroma per index (signed, centered 0)
static unsigned short g_hx[VP_W];        // viewport column -> DOOM column
static unsigned char  g_vy[VP_H];        // viewport row    -> DOOM row
static int g_cleared = 0;

static void cb_setpal(const unsigned char *rgb) {
    int i;
    for (i = 0; i < 256; i++) {
        int r = rgb[i*3], g = rgb[i*3+1], b = rgb[i*3+2];
        int y = (77*r + 150*g + 29*b) >> 8;     // 0.299R + 0.587G + 0.114B
        int u = ((b - y) * 144) >> 8;            // (B-Y)/1.772
        int v = ((r - y) * 183) >> 8;            // (R-Y)/1.402
        if (y < 0) y = 0; else if (y > 255) y = 255;
        if (u < -128) u = -128; else if (u > 127) u = 127;
        if (v < -128) v = -128; else if (v > 127) v = 127;
        g_y[i] = (unsigned char)y; g_u[i] = (signed char)u; g_v[i] = (signed char)v;
    }
}
static void cb_draw(const unsigned char *fb, int w, int h) {
    unsigned char *vp = VP_FB_D;
    if (!vp) return;
    if (!g_cleared) {                            // first frame: hide the UI overlay, build scale maps
        unsigned char *osd = vid_get_bitmap_fb();
        if (osd) { int n = camera_screen.buffer_width * camera_screen.height, k; for (k = 0; k < n; k++) osd[k] = 0; }
        int c; for (c = 0; c < VP_W; c++) g_hx[c] = (unsigned short)(c * w / VP_W);
        int r; for (r = 0; r < VP_H; r++) g_vy[r] = (unsigned char)(r * h / VP_H);
        g_cleared = 1;
    }
    int r;
    for (r = 0; r < VP_H; r++) {
        const unsigned char *s = fb + g_vy[r] * w;
        unsigned char *d = vp + r * VP_BW;
        int g;
        for (g = 0; g < VP_W; g += 4) {          // 4 viewport px = one UYVYYY group = 6 bytes
            unsigned int q0 = s[g_hx[g]], q1 = s[g_hx[g+1]], q2 = s[g_hx[g+2]], q3 = s[g_hx[g+3]];
            d[0] = (unsigned char)(((int)g_u[q0] + g_u[q1] + g_u[q2] + g_u[q3]) >> 2);  // shared U
            d[1] = g_y[q0];
            d[2] = (unsigned char)(((int)g_v[q0] + g_v[q1] + g_v[q2] + g_v[q3]) >> 2);  // shared V
            d[3] = g_y[q1];
            d[4] = g_y[q2];
            d[5] = g_y[q3];
            d += 6;
        }
    }
}

// Input: poll the physical keys (Canon's kbd task keeps these live even while we sit in the
// blob's loop) and emit DOOM key transitions one per call. Round-robin scan so no key starves.
#define DK_RIGHT 0xae
#define DK_LEFT  0xac
#define DK_UP    0xad
#define DK_DOWN  0xaf
#define DK_STRL  0xa0
#define DK_STRR  0xa1
#define DK_USE   0xa2
#define DK_FIRE  0xa3
#define DK_ESC   27
#define DK_ENTER 13
#define DK_RUN   (0x80 + 0x36)
static const struct { long ck; unsigned char dk; } KEYMAP[] = {
    // THUMB (back): d-pad = pure movement, SET = use/confirm
    { KEY_UP,         DK_UP    },   // forward
    { KEY_DOWN,       DK_DOWN  },   // back
    { KEY_LEFT,       DK_STRL  },   // strafe left
    { KEY_RIGHT,      DK_STRR  },   // strafe right
    { KEY_SET,        DK_USE   },   // open doors / use (in game)
    { KEY_SET,        DK_ENTER },   // confirm (in menus) - SET doubles as both
    // INDEX FINGER (top): shutter = fire, zoom rocker = turn
    { KEY_SHOOT_FULL, DK_FIRE  },   // fire the gun (the trigger - natural)
    { KEY_SHOOT_HALF, DK_RUN   },   // light squeeze = run
    { KEY_ZOOM_IN,    DK_RIGHT },   // turn right
    { KEY_ZOOM_OUT,   DK_LEFT  },   // turn left
    { KEY_MENU,       DK_ESC   },   // DOOM menu
    { KEY_DISPLAY,    DK_ENTER },   // confirm
};
#define NKEYS (int)(sizeof(KEYMAP) / sizeof(KEYMAP[0]))
static unsigned char g_prev[NKEYS];
static int g_scan = 0;
static int cb_getkey(int *pressed, unsigned char *key) {
    int n;
    for (n = 0; n < NKEYS; n++) {
        int i = g_scan; g_scan = (g_scan + 1) % NKEYS;
        unsigned char now = kbd_is_key_pressed(KEYMAP[i].ck) ? 1 : 0;
        if (now != g_prev[i]) {
            g_prev[i] = now;
            *pressed = now;
            *key = KEYMAP[i].dk;
            return 1;
        }
    }
    return 0;
}
// Quit: the PRINT / share button (the blue one) is unused by DOOM, so it's our "get me out".
// Blob polls this each tick and returns from doom_main when it goes nonzero.
static int cb_should_quit(void) { return kbd_is_key_pressed(KEY_PRINT) ? 1 : 0; }
static int cb_open(const char *path, int wr) {
    // Flatten every DOOM path to the card root: "A/<basename>". DOOM builds save/config paths that
    // can include a subdir (e.g. ".savegame/doomsav0.dsg") which can't exist on the card (mkdir is a
    // stub), so opening them fails and DOOM I_Errors to a blank screen. Drop the directory, keep the
    // filename. Writes get O_CREAT|O_TRUNC so the save file is actually created.
    const char *base = path, *q;
    for (q = path; *q; q++) if (*q == '/' || *q == '\\') base = q + 1;
    char buf[80];
    int i = 0; buf[0] = 'A'; buf[1] = '/';
    while (base[i] && i < 76) { buf[i + 2] = base[i]; i++; }
    buf[i + 2] = 0;
    return open(buf, wr ? (O_WRONLY | O_CREAT | O_TRUNC) : O_RDONLY, 0777);
}
static int cb_read(int fd, void *b, int n)   { return read(fd, b, n); }
static int cb_write(int fd, const void *b, int n) { return write(fd, (void *)b, n); }
static int cb_lseek(int fd, int off, int wh) { return lseek(fd, off, wh); }
static int cb_close(int fd)                  { return close(fd); }

// --- the launch sequence -----------------------------------------------------
static void launch(void) {
    char msg[48];
    draw_rectangle(camera_screen.disp_left, 0, camera_screen.disp_left + camera_screen.disp_width - 1,
                   camera_screen.height - 1, MAKE_COLOR(COLOR_BLACK, COLOR_BLACK), RECT_BORDER0 | DRAW_FILLED);
    scr_log("DOOM LAUNCHER");

    // hook_raw_image_addr() hands back the UNCACHED alias (0x41xxxxxx on DIGIC 4); strip the alias bit so
    // every address we compute (blob load, arena) lives in the CACHED alias we now execute from.
    unsigned int raw = (unsigned int)hook_raw_image_addr() & ~UNCACHED_BIT;
    sprintf(msg, "raw=0x%08x", raw); scr_log(msg);
    if (raw == 0 || raw > BLOB_ADDR || BLOB_ADDR >= raw + RAW_SIZE) {
        sprintf(msg, "bad raw 0x%08x - abort", raw); scr_log(msg); return;
    }
    if (vid_get_viewport_byte_width() != VP_BW) {
        sprintf(msg, "vp stride %d != %d - abort", vid_get_viewport_byte_width(), VP_BW); scr_log(msg); return;
    }

    scr_log("loading A/DOOM.BIN ...");
    int fd = open("A/DOOM.BIN", O_RDONLY, 0777);
    if (fd < 0) { scr_log("DOOM.BIN NOT FOUND on card"); return; }
    unsigned char *blob = (unsigned char *)BLOB_ADDR;
    int total = 0, r;
    while ((r = read(fd, blob + total, 65536)) > 0) total += r;
    close(fd);
    if (total < 100000) { sprintf(msg, "blob too small (%d) - abort", total); scr_log(msg); return; }

    unsigned int entry    = *(volatile unsigned int *)(BLOB_ADDR);       // header word0 = entry
    unsigned int blob_end = *(volatile unsigned int *)(BLOB_ADDR + 4);   // header word1 = arena start
    // sanity: entry + arena must sit inside the loaded blob / raw buffer, else a corrupt load would
    // jump into garbage and hang. Bail with a visible message instead.
    if (entry < BLOB_ADDR || entry >= BLOB_ADDR + (unsigned)total ||
        blob_end <= BLOB_ADDR || blob_end >= raw + RAW_SIZE) {
        sprintf(msg, "bad blob hdr e=%08x - abort", entry); scr_log(msg); return;
    }
    sprintf(msg, "loaded %d, entry=0x%08x", total, entry); scr_log(msg);

    cb.magic        = DOOM_ABI_MAGIC;
    cb.get_ticks_ms = cb_ticks;
    cb.sleep_ms     = cb_sleep;
    cb.get_key      = cb_getkey;
    cb.draw_frame   = cb_draw;
    cb.set_palette  = cb_setpal;
    cb.f_open = cb_open; cb.f_read = cb_read; cb.f_write = cb_write;
    cb.f_lseek = cb_lseek; cb.f_close = cb_close;
    cb.arena      = (char *)blob_end;
    cb.arena_size = (raw + RAW_SIZE) - blob_end;
    cb.log        = scr_log;
    cb.should_quit = cb_should_quit;
    sprintf(msg, "arena=%u KB", cb.arena_size / 1024); scr_log(msg);

    scr_log("flush + JUMP ->");
    dcache_clean_all();          // push freshly written code from D-cache to RAM
    icache_invalidate_all();     // drop stale I-cache lines so the new code is fetched
    ((int (*)(doom_cb_t *))entry)(&cb);     // returns when the user presses PRINT to quit
}

//-------------------------------------------------------------------
int gui_doom_kbd_process(void) { kbd_get_clicked_key(); return 0; }

void gui_doom_draw(void) {
    if (launched) return;
    launched = 1;
    launch();                      // blocks in the DOOM tick loop; returns only on PRINT (quit) or a boot abort
    // Clean exit: leave ALT mode entirely. The core loop's gui_activate_alt_mode() then unloads the
    // module (running=0) and returns to defaultGuiHandler, so Canon repaints the real Playback photo
    // instead of leaving DOOM's last frame on screen. Refresh the overlay too so nothing lingers.
    vid_bitmap_refresh();
    gui_set_need_restore();
    exit_alt();
}

int gui_doom_init(void) {
    launched = 0; logy = 4; g_cleared = 0;
    gui_set_mode(&GUI_MODE_DOOM);
    return 1;
}
int basic_module_init(void) { return gui_doom_init(); }

#include "simple_game.c"

/******************** Module Information structure ******************/

ModuleInfo _module_info =
{
    MODULEINFO_V1_MAGICNUM,
    sizeof(ModuleInfo),
    SIMPLE_MODULE_VERSION,

    ANY_CHDK_BRANCH, 0, OPT_ARCHITECTURE,
    ANY_PLATFORM_ALLOWED,

    (int)"DOOM",
    MTYPE_GAME,

    &_librun.base,

    ANY_VERSION,
    CAM_SCREEN_VERSION,
    ANY_VERSION,
    ANY_VERSION,

    0,
};

/*************** END OF AUXILARY PART *******************/
