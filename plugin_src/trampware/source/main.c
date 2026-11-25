// Trampware - black/white in-game overlay menu for GoldHEN.
//
// Open / close:  press L3 + R3 (toggle).  CIRCLE (O) closes the menu from anywhere.  (Controls tab can switch to L1 + R1 + OPTIONS)
// Navigate:      D-Pad UP/DOWN move, LEFT/RIGHT adjust, CROSS (X) enter / click / open, TRIANGLE back to categories,
//                L1/R1 switch tab, SQUARE reset the selected option
//
// The menu is software-rendered (render.c) on a background render thread and composited onto the game's
// video-out buffers by a background draw thread, so neither the controller nor the game thread ever waits on it.
// Settings are stored in /data/GoldHEN/trampware.ini.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <stdint.h>
#include <stdbool.h>

#include <Common.h>
#include <Patcher.h>
#include <orbis/VideoOut.h>
#include <orbis/libkernel.h>
#include "plugin_common.h"
#include "pad.h"
#include "render.h"
#include "tile.h"

attr_public const char* g_pluginName = "trampware";
attr_public const char* g_pluginDesc = "Trampware black/white overlay menu";
attr_public const char* g_pluginAuth = "Trampware";
attr_public u32 g_pluginVersion = 0x00000210;  // 2.10

#define CONFIG_PATH GOLDHEN_PATH "/trampware.ini"
#define COMBO_L3R3 (SCE_PAD_BUTTON_L3 | SCE_PAD_BUTTON_R3)
#define COMBO_ALT  (SCE_PAD_BUTTON_L1 | SCE_PAD_BUTTON_R1 | SCE_PAD_BUTTON_OPTIONS)
#define OPEN_COMBO open_combo()
// Flip modes passed to sceVideoOutSubmitFlip / sceGnmSubmitAndFlipCommandBuffers when the V-Sync override is on.
// 1 = wait for vertical blank (V-Sync on), 0 = flip immediately (V-Sync off). These are the values this build assumes;
// if V-Sync does not change on the console, these two numbers are the ones to edit.
#define FLIP_MODE_VSYNC_ON  1
#define FLIP_MODE_VSYNC_OFF 0
#define NAV_MASK (SCE_PAD_BUTTON_UP | SCE_PAD_BUTTON_DOWN | SCE_PAD_BUTTON_LEFT | SCE_PAD_BUTTON_RIGHT | \
                  SCE_PAD_BUTTON_CROSS | SCE_PAD_BUTTON_CIRCLE | SCE_PAD_BUTTON_L1 | SCE_PAD_BUTTON_R1)

// Option indices (see kDefaults in render.c)
enum { V_ENABLED, V_COMPACT, V_OPACITY, V_SCALE, V_ACCENT };
enum { H_ENABLED, H_CROSSHAIR, H_STATUS, H_SCALE, H_VSYNC_OVERRIDE, H_VSYNC };
enum { C_BLOCK, C_VIBRATION, C_SPEED, C_HINTS, C_TRIGGER };
enum { S_LOGGING, S_TILED, S_NOTIFY, S_FPS_UNLOCK, S_RESET, S_ABOUT };

HOOK_INIT(scePadRead);
HOOK_INIT(scePadReadState);
HOOK_INIT(scePadSetVibration);
HOOK_INIT(sceVideoOutRegisterBuffers);
HOOK_INIT(sceVideoOutSubmitFlip);
HOOK_INIT(sceGnmSubmitAndFlipCommandBuffers);
HOOK_INIT(sceVideoOutSetFlipRate);

// Most PS4 games present through the graphics driver, not sceVideoOutSubmitFlip (same hook as frame_logger).
s32 sceGnmSubmitAndFlipCommandBuffers(u32 count, void* dcbGpuAddrs[], u32* dcbSizesInBytes, void* ccbGpuAddrs[],
                                      u32* ccbSizesInBytes, u32 videoOutHandle, u32 displayBufferIndex, u32 flipMode, s64 flipArg);
static volatile bool g_gnmSeen;   // once the Gnm flip path is seen, the VideoOut hook stops drawing
int scePthreadSetprio();   // (thread, prio): lower number = higher priority
static volatile s32 g_prevIdx = -1;

// The stock scePadRead / scePadReadState are tiny stubs that tail-call scePadReadExt / scePadReadStateExt.
// Their first bytes are not safe to copy into a hook trampoline, so calling the "original" through
// HOOK_CONTINUE jumps into garbage and crashes the game on the first controller read.
// Same approach as gamepad_helper: patch the Ext functions and call them directly instead.
static Patcher* g_padReadExtPatcher;
static Patcher* g_padReadStateExtPatcher;
static volatile bool g_ready;   // hooks only act after plugin_load finished initialising

// Layout of the leading fields of OrbisVideoOutBufferAttribute
typedef struct { s32 format; s32 tilingMode; s32 aspect; u32 width; u32 height; u32 pitch; } FbAttr;
#define FB_FMT_A8R8G8B8 0x80000000
#define FB_FMT_A8B8G8R8 0x80002200
#define FB_TILING_LINEAR 1
#define MAX_FB 32

static struct { void* addr; } g_fb[MAX_FB];
static FbAttr g_fbAttr;
static bool g_fbKnown;
static bool g_overlayOk;            // true once the overlay was actually drawn on a supported framebuffer
static char g_loadMsg[96] = "";     // custom load notification (message= in trampware.ini)

static TwMenu g_menu;
static bool g_open;
static bool g_dirty = true;         // menu content / look changed -> render thread redraws every layer
static bool g_saveReq;              // settings must be written (done on the render thread, never on the pad thread)
static volatile int g_lock;         // protects g_menu, g_open, g_dirty, g_toast*, g_saveReq (only ever held briefly)
static volatile int g_cvLock;       // protects the layer canvases + per-buffer draw state
static volatile bool g_run;
static char g_titleid[16] = "";

// toast
static char g_toast[64];
static u64 g_toastUntil;

// ---- layers: each one has a front canvas (shown) and a back canvas (being rendered); swapped under g_cvLock
enum { L_STATUS, L_CROSS, L_PANEL, L_TOAST, L_COUNT };
typedef struct {
    TwCanvas cv[2];
    int front;
    bool visible;
    int x, y, alpha;
    // Pre-converted pixel list for the front canvas: framebuffer dword index + final colour. Drawing is then a plain
    // store per pixel (no blending, no reads), so it can be repeated every millisecond without any side effects.
    u32 *idx, *col;     // list the draw thread reads (only swapped while holding g_cvLock)
    int n, cap;
    u32 *nidx, *ncol;   // spare list the render thread builds into WITHOUT holding the lock
    int ncap;
    u32 geo;            // framebuffer geometry version the list was built for
} Layer;
static Layer g_layer[L_COUNT];

static volatile u32 g_geo = 1;              // bumped when the framebuffers are (re)registered
static volatile s32 g_curIdx = -1;          // buffer submitted last (still being rendered / about to be shown)
static volatile u64 g_curMs;                // when g_curIdx was submitted (used to let the Gnm path finish drawing)

static OrbisPthread g_thRender, g_thDraw;
static bool g_threadsOn;

// fps
static u64 g_fpsT0;
static int g_fpsFrames;
static volatile float g_fps;

// ------------------------------------------------------------------ helpers
static u32 open_combo(void);   // defined after the options are available
static inline void lock(void) { while (__atomic_exchange_n(&g_lock, 1, __ATOMIC_ACQUIRE)) {} }
static inline void unlock(void) { __atomic_store_n(&g_lock, 0, __ATOMIC_RELEASE); }
// canvas lock: may be held for the length of one blit, so waiters sleep instead of burning the core
static inline void cvlock(void) { while (__atomic_exchange_n(&g_cvLock, 1, __ATOMIC_ACQUIRE)) sceKernelUsleep(50); }
static inline void cvunlock(void) { __atomic_store_n(&g_cvLock, 0, __ATOMIC_RELEASE); }

static u64 now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000ull + (u64)ts.tv_nsec / 1000000ull;
}

#define tw_log(fmt, args...) do { if (g_menu.opts[TW_TAB_SYSTEM][S_LOGGING].v) final_printf("[trampware] " fmt, ##args); } while (0)
#define OPT(tab, i) (g_menu.opts[tab][i])
// Open/close combo: L3 + R3 (default) or L1 + R1 + OPTIONS
static u32 open_combo(void) { return OPT(TW_TAB_CONTROLS, C_TRIGGER).v ? COMBO_L3R3 : COMBO_ALT; }

// Applies the V-Sync toggle (HUD tab) to the flip mode the game asks for. Override off = the game's own choice is kept.
static u32 vsync_flip(u32 flipMode) {
    if (!g_ready || !OPT(TW_TAB_HUD, H_VSYNC_OVERRIDE).v) return flipMode;
    return OPT(TW_TAB_HUD, H_VSYNC).v ? FLIP_MODE_VSYNC_ON : FLIP_MODE_VSYNC_OFF;
}

static void toast(const char* text) {
    snprintf(g_toast, sizeof(g_toast), "%s", text);
    g_toastUntil = now_ms() + 1300;
    g_dirty = true;
}

// ------------------------------------------------------------------ config
static void save_config(const TwMenu* m) {
    FILE* f = fopen(CONFIG_PATH, "w");
    if (!f) return;
    for (int t = 0; t < TW_TAB_COUNT; t++)
        for (int i = 0; i < m->count[t]; i++)
            if (m->opts[t][i].type != TW_ACTION) fprintf(f, "%d.%d=%d\n", t, i, m->opts[t][i].v);
    fprintf(f, "message=%s\n", g_loadMsg);   // edit this line to change the loading notification
    fclose(f);
}

static bool load_config(void) {
    FILE* f = fopen(CONFIG_PATH, "r");
    if (!f) return false;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "message=", 8) == 0) {
            snprintf(g_loadMsg, sizeof(g_loadMsg), "%s", line + 8);
            size_t n = strlen(g_loadMsg);
            while (n && (g_loadMsg[n - 1] == '\n' || g_loadMsg[n - 1] == '\r')) g_loadMsg[--n] = 0;
            continue;
        }
        int t, i, v;
        if (sscanf(line, "%d.%d=%d", &t, &i, &v) == 3 && t >= 0 && t < TW_TAB_COUNT && i >= 0 && i < g_menu.count[t]) {
            TwOpt* o = &g_menu.opts[t][i];
            if (o->type == TW_ACTION) continue;
            o->v = v < o->min ? o->min : (v > o->max ? o->max : v);
        }
    }
    fclose(f);
    return true;
}

// Copy the look-and-feel options into the menu struct used by the renderer
static void sync_look(void) {
    g_menu.compact = OPT(TW_TAB_VISUALS, V_COMPACT).v;
    g_menu.accent = OPT(TW_TAB_VISUALS, V_ACCENT).v;
    g_menu.hints = OPT(TW_TAB_CONTROLS, C_HINTS).v;
}

// ------------------------------------------------------------------ menu logic
static void set_open(bool open);

static void change(TwOpt* o, int dir, bool announce) {
    char msg[64];
    if (o->type == TW_TOGGLE) {
        o->v = !o->v;
        snprintf(msg, sizeof(msg), "%s: %s", o->label, o->v ? "ON" : "OFF");
        if (announce) toast(msg);
    } else if (o->type == TW_SLIDER) {
        int step = (o->max - o->min) / 20; if (step < 1) step = 1;
        if (o->max - o->min <= 10) step = 1;
        o->v += dir * step;
        if (o->v < o->min) o->v = o->min;
        if (o->v > o->max) o->v = o->max;
    }
    sync_look();
    g_dirty = true;
    if (o == &OPT(TW_TAB_VISUALS, V_ENABLED) && !o->v) set_open(false);   // hide the menu; the combo brings it back
}

static void activate(TwOpt* o) {
    if (o->type == TW_TOGGLE) { change(o, 0, true); return; }
    if (o->type != TW_ACTION) return;
    if (&OPT(TW_TAB_SYSTEM, S_RESET) == o) {
        tw_menu_reset(&g_menu);
        sync_look();
        toast("Settings reset");
    } else {
        toast("Trampware menu · build 0.1.0");
    }
    g_dirty = true;
}

static void set_open(bool open) {
    if (g_open == open) return;
    g_open = open;
    g_menu.focus = 0;   // always start on the category list
    if (open) OPT(TW_TAB_VISUALS, V_ENABLED).v = 1;
    else g_saveReq = true;   // written by the render thread, so closing is instant
    g_dirty = true;
    tw_log("menu %s\n", open ? "opened" : "closed");
}

// Handles one pad sample. Returns true if the sample should be hidden from the game.
static u32 g_prevBtn, g_swallow;
static bool g_comboHeld;   // was the open combo fully held on the previous sample
static u64 g_nextRepeat;

static bool menu_input(u32 btn) {
    u32 pressed = btn & ~g_prevBtn;
    g_prevBtn = btn;

    // Open/close: L3 + R3 together (or L1+R1+OPTIONS if switched in Controls). The toggle fires on the sample where the
    // whole combo becomes held, so it works whether the two sticks go down together or a moment apart, press-and-hold
    // opens it once, and pressing the combo again toggles it. CIRCLE (O) also closes.
    const u32 oc = open_combo();
    const bool comboHeld = (btn & oc) == oc;
    const bool combo = comboHeld && !g_comboHeld;
    g_comboHeld = comboHeld;
    if (combo) {
        set_open(!g_open);
        g_swallow = btn;   // ignore everything held right now until it is released
        return true;
    }
    g_swallow &= btn;
    if (!g_open) return g_swallow != 0;   // keep hiding the opening press from the game until it is released

    u32 b = btn & ~g_swallow;
    u32 p = pressed & ~g_swallow;
    u64 t = now_ms();
    TwOpt* cur = &g_menu.opts[g_menu.tab][g_menu.selected];

    // L1 / R1 still jump between tabs from anywhere
    if (p & SCE_PAD_BUTTON_L1) { g_menu.tab = (g_menu.tab + TW_TAB_COUNT - 1) % TW_TAB_COUNT; g_menu.selected = 0; g_dirty = true; }
    if (p & SCE_PAD_BUTTON_R1) { g_menu.tab = (g_menu.tab + 1) % TW_TAB_COUNT; g_menu.selected = 0; g_dirty = true; }
    cur = &g_menu.opts[g_menu.tab][g_menu.selected];   // tab may have just changed

    // O (CIRCLE) closes the menu from anywhere. The press is kept from the game until it is released.
    if (p & SCE_PAD_BUTTON_CIRCLE) {
        set_open(false);
        g_swallow = btn;
        return true;
    }

    if (g_menu.focus == 0) {
        // ---- category list: D-Pad moves between categories, X opens the category
        if (p & SCE_PAD_BUTTON_UP)   { g_menu.tab = (g_menu.tab + TW_TAB_COUNT - 1) % TW_TAB_COUNT; g_menu.selected = 0; g_dirty = true; }
        if (p & SCE_PAD_BUTTON_DOWN) { g_menu.tab = (g_menu.tab + 1) % TW_TAB_COUNT; g_menu.selected = 0; g_dirty = true; }
        if (p & (SCE_PAD_BUTTON_CROSS | SCE_PAD_BUTTON_RIGHT)) { g_menu.focus = 1; g_menu.selected = 0; g_dirty = true; }
    } else {
        // ---- options: D-Pad moves (hold to repeat), LEFT/RIGHT adjusts, X enters / clicks, TRIANGLE goes back
        if (p & SCE_PAD_BUTTON_TRIANGLE) { g_menu.focus = 0; g_dirty = true; }
        if (p & SCE_PAD_BUTTON_CROSS) activate(cur);
        if ((p & SCE_PAD_BUTTON_SQUARE) && cur->type != TW_ACTION) {   // SQUARE = reset the selected option
            cur->v = cur->def; sync_look(); g_dirty = true; toast("Reset to default");
        }
        u32 dirs = b & (SCE_PAD_BUTTON_UP | SCE_PAD_BUTTON_DOWN | SCE_PAD_BUTTON_LEFT | SCE_PAD_BUTTON_RIGHT);
        bool fire = false;
        if (p & (SCE_PAD_BUTTON_UP | SCE_PAD_BUTTON_DOWN | SCE_PAD_BUTTON_LEFT | SCE_PAD_BUTTON_RIGHT)) { fire = true; g_nextRepeat = t + 350; }
        else if (dirs && t >= g_nextRepeat) { fire = true; g_nextRepeat = t + (u64)(240 - OPT(TW_TAB_CONTROLS, C_SPEED).v * 20); }
        if (fire) {
            int n = g_menu.count[g_menu.tab];
            if (dirs & SCE_PAD_BUTTON_UP) { g_menu.selected = (g_menu.selected + n - 1) % n; g_dirty = true; }
            else if (dirs & SCE_PAD_BUTTON_DOWN) { g_menu.selected = (g_menu.selected + 1) % n; g_dirty = true; }
            else if (dirs & (SCE_PAD_BUTTON_LEFT | SCE_PAD_BUTTON_RIGHT)) {
                int dir = (dirs & SCE_PAD_BUTTON_RIGHT) ? 1 : -1;
                TwOpt* o = &g_menu.opts[g_menu.tab][g_menu.selected];
                if (o->type == TW_TOGGLE) { if (p & (SCE_PAD_BUTTON_LEFT | SCE_PAD_BUTTON_RIGHT)) change(o, 0, true); }
                else change(o, dir, false);
            }
        }
    }
    return OPT(TW_TAB_CONTROLS, C_BLOCK).v != 0;
}

static void hide_from_game(ScePadData* d) {
    d->buttons = (d->buttons & SCE_PAD_BUTTON_INTERCEPTED);
    d->leftStick.x = d->leftStick.y = d->rightStick.x = d->rightStick.y = 0x80;
    memset(&d->analogButtons, 0, sizeof(d->analogButtons));
}

static void process_pad(ScePadData* data, int count) {
    for (int i = count - 1; i >= 0; i--) {   // oldest sample first
        lock();
        bool hide = menu_input(data[i].buttons);
        bool open = g_open;
        unlock();
        // never leak the open/close combo to the game, and hide everything while the menu has focus
        if (hide || (data[i].buttons & OPEN_COMBO) == OPEN_COMBO) hide_from_game(&data[i]);
        else if (open && OPT(TW_TAB_CONTROLS, C_BLOCK).v) hide_from_game(&data[i]);
    }
}

s32 scePadRead_hook(s32 handle, ScePadData* data, s32 num) {
    s32 ret = scePadReadExt(handle, data, num);
    if (g_ready && data && ret > 0) process_pad(data, ret);
    return ret;
}

s32 scePadReadState_hook(s32 handle, ScePadData* data) {
    s32 ret = scePadReadStateExt(handle, data);
    if (g_ready && data && ret == 0) process_pad(data, 1);
    return ret;
}

s32 scePadSetVibration_hook(s32 handle, const ScePadVibrationParam* param) {
    if (g_ready && !OPT(TW_TAB_CONTROLS, C_VIBRATION).v) return 0;
    return HOOK_CONTINUE(scePadSetVibration, s32 (*)(s32, const ScePadVibrationParam*), handle, param);
}

// FPS unlocker: games ask for a frame-rate cap with sceVideoOutSetFlipRate. With the option on, the request is dropped
// and the game runs uncapped. The plugin loads before the game starts, so it sees the game's startup request.
// Restart the game after changing the option, because the cap is only set when the game asks for it.
s32 sceVideoOutSetFlipRate_hook(s32 handle, s32 fliprate) {
    if (g_ready && OPT(TW_TAB_SYSTEM, S_FPS_UNLOCK).v) return 0;
    return HOOK_CONTINUE(sceVideoOutSetFlipRate, s32 (*)(s32, s32), handle, fliprate);
}

// ------------------------------------------------------------------ compositing
// Pixel index inside the framebuffer. Linear: row-major. Tiled: PS4 2D-thin display tiling, see tile.h.
static bool g_tiledDraw;
static unsigned g_macroPitch = 15;
static inline size_t fb_index(int x, int y, int pitch) {
    if (!g_tiledDraw) return (size_t)y * pitch + x;
    return tw_tiled_index((unsigned)x, (unsigned)y, g_macroPitch);
}

static bool ensure(TwCanvas* c, int w, int h) {
    if (c->px && c->w == w && c->h == h) return true;
    tw_canvas_free(c);
    return tw_canvas_alloc(c, w, h);
}

// Is the registered framebuffer something we can draw on? Also primes the tiling globals.
static bool fb_supported(void) {
    if (!g_ready || !g_fbKnown) return false;
    const bool fmtOk = (g_fbAttr.format == FB_FMT_A8R8G8B8 || g_fbAttr.format == FB_FMT_A8B8G8R8);
    const bool linear = (g_fbAttr.tilingMode == FB_TILING_LINEAR);
    const bool tiledOn = OPT(TW_TAB_SYSTEM, S_TILED).v != 0;
    const u32 pitch = g_fbAttr.pitch ? g_fbAttr.pitch : g_fbAttr.width;
    g_tiledDraw = !linear;
    g_macroPitch = (pitch + 127) / 128;
    const bool dimsOk = linear || (pitch >= g_fbAttr.width);
    g_overlayOk = fmtOk && dimsOk && (linear || tiledOn);
    return g_overlayOk;
}

// ---- layer publishing (render thread -> draw thread)
// Convert the front canvas to a flat list of (framebuffer index, colour). Pixels that are mostly transparent are skipped,
// the rest are stored fully opaque, so the menu and watermark are always drawn at full strength.
// Build the pixel list of the front canvas into the spare list. Runs on the render thread WITHOUT the canvas lock, so
// the draw thread is never kept waiting while a menu list is being prepared (that waiting was what made the overlay drop
// out for a moment on every option change).
static int layer_build(Layer* L, const FbAttr a, int x, int y) {
    const TwCanvas* c = &L->cv[L->front];
    const int W = (int)a.width, H = (int)a.height;
    const int pitch = a.pitch ? (int)a.pitch : W;
    const bool swapRB = (a.format == FB_FMT_A8B8G8R8);
    if (!c->px) return 0;
    const int need = c->w * c->h;
    if (need > L->ncap) {
        free(L->nidx); free(L->ncol);
        L->nidx = (u32*)malloc((size_t)need * 4);
        L->ncol = (u32*)malloc((size_t)need * 4);
        L->ncap = (L->nidx && L->ncol) ? need : 0;
        if (!L->ncap) return 0;
    }
    int n = 0;
    for (int j = 0; j < c->h; j++) {
        const int yy = y + j;
        if (yy < 0 || yy >= H) continue;
        for (int i = 0; i < c->w; i++) {
            const u32 s = c->px[(size_t)j * c->w + i];
            if ((s >> 24) < 96) continue;          // mostly transparent: skip (menu pixels are stored fully opaque)
            const int xx = x + i;
            if (xx < 0 || xx >= W) continue;
            u32 r = (s >> 16) & 255, g = (s >> 8) & 255, b = s & 255;
            if (swapRB) { u32 t = r; r = b; b = t; }
            L->nidx[n] = (u32)fb_index(xx, yy, pitch);
            L->ncol[n] = 0xFF000000u | (r << 16) | (g << 8) | b;
            n++;
        }
    }
    return n;
}

// Make `li` show the given state. swap = the back canvas was just rendered and becomes the front one.
static void layer_publish(int li, bool vis, int x, int y, bool swap) {
    Layer* L = &g_layer[li];
    if (!swap && L->visible == vis && L->x == x && L->y == y && L->geo == g_geo) return;
    if (!fb_supported()) return;    // primes the tiling globals used by layer_build
    if (swap) L->front ^= 1;
    const bool show = vis && L->cv[L->front].px != NULL;
    const FbAttr a = g_fbAttr;      // a torn read is harmless: a geometry change bumps g_geo and the list is rebuilt
    const u32 geo = g_geo;
    const int n = show ? layer_build(L, a, x, y) : 0;
    cvlock();
    L->visible = show;
    L->x = x; L->y = y;
    if (show && n > 0) {
        u32 *ti = L->idx, *tc = L->col;
        L->idx = L->nidx; L->col = L->ncol; L->nidx = ti; L->ncol = tc;
        int tcap = L->cap; L->cap = L->ncap; L->ncap = tcap;
        L->n = n;
        L->geo = geo;
    } else if (show) {
        L->n = 0;
        L->geo = geo;
    }
    cvunlock();
}

// ---- drawing onto one framebuffer
static void draw_buffer(int idx) {
    u32* fbp = (u32*)g_fb[idx].addr;
    for (int li = 0; li < L_COUNT; li++) {
        const Layer* L = &g_layer[li];
        if (!L->visible || L->geo != g_geo) continue;
        const u32 *ix = L->idx, *cl = L->col;
        for (int k = 0, n = L->n; k < n; k++) fbp[ix[k]] = cl[k];
    }
}

static void service_buffer(int idx) {
    if (idx < 0 || idx >= MAX_FB || !g_fb[idx].addr || !fb_supported()) return;
    cvlock();
    draw_buffer(idx);
    cvunlock();
}

// Draw thread: the game repaints every buffer from scratch every frame and we cannot tell when the GPU is done with one,
// so the overlay is simply stamped onto every buffer over and over (a few hundred times a second). The stamp is a plain
// store of the same pixels, so repeating it is free of side effects, and whichever buffer is on screen always has it.
static void* draw_thread(void* arg) {
    (void)arg;
    while (g_run) {
        sceKernelUsleep(100);
        if (!g_ready || !g_fbKnown || !fb_supported()) continue;
        // the two buffers that matter right now (on screen / about to be shown) are stamped first and twice as often
        const s32 cur = g_curIdx, prev = g_prevIdx;
        const bool curSettled = !g_gnmSeen || (now_ms() - g_curMs) >= 6;
        static int pass;
        cvlock();
        if (prev >= 0 && prev < MAX_FB && g_fb[prev].addr) draw_buffer(prev);
        if (curSettled && cur >= 0 && cur < MAX_FB && cur != prev && g_fb[cur].addr) draw_buffer(cur);
        if ((++pass & 7) == 0)   // the other buffers are not on screen; touching them rarely keeps the CPU free
            for (int i = 0; i < MAX_FB; i++)
                if (g_fb[i].addr && i != cur && i != prev) draw_buffer(i);
        cvunlock();
    }
    scePthreadExit(NULL);
    return NULL;
}

// Render thread: everything slow lives here (software rendering of the panel, status box, toast, writing the ini).
static void* render_thread(void* arg) {
    (void)arg;
    char lastL2[48] = "", lastToast[64] = "";
    float lastStatusS = -1, lastCrossS = -1, lastPanelS = -1;
    int lastAccent = -1;
    bool first = true;

    while (g_run) {
        sceKernelUsleep(4000);
        if (!g_ready || !g_fbKnown || !g_fbAttr.width || !g_fbAttr.height) continue;
        const int W = (int)g_fbAttr.width, H = (int)g_fbAttr.height;
        const u64 t = now_ms();

        // snapshot the shared state, then work without holding the lock
        lock();
        TwMenu m = g_menu;
        const bool open = g_open && m.opts[TW_TAB_VISUALS][V_ENABLED].v;
        const bool dirty = g_dirty || first;
        g_dirty = false;
        const bool saveReq = g_saveReq;
        g_saveReq = false;
        bool toastOn = g_toast[0] && t < g_toastUntil;
        if (!toastOn && g_toast[0]) g_toast[0] = 0;
        char toastText[64];
        snprintf(toastText, sizeof(toastText), "%s", g_toast);
        unlock();
        first = false;

        if (saveReq) save_config(&m);

        const bool hud = m.opts[TW_TAB_HUD][H_ENABLED].v != 0;
        const int accent = m.opts[TW_TAB_VISUALS][V_ACCENT].v;
        const bool lookChanged = accent != lastAccent;
        lastAccent = accent;

        // --- status box (watermark)
        {
            Layer* L = &g_layer[L_STATUS];
            const bool vis = hud && m.opts[TW_TAB_HUD][H_STATUS].v;
            float s = m.opts[TW_TAB_HUD][H_SCALE].v / 100.0f * ((float)H / 1080.0f < 0.67f ? 0.8f : 1.0f);
            char l2[48];
            snprintf(l2, sizeof(l2), "%s   %d FPS", g_titleid[0] ? g_titleid : "GAME", (int)(g_fps + 0.5f));
            bool swap = false;
            if (vis && (strcmp(l2, lastL2) || s != lastStatusS || lookChanged || !L->cv[L->front].px)) {
                int w, h; tw_status_size(s, &w, &h);
                TwCanvas* back = &L->cv[L->front ^ 1];
                if (ensure(back, w, h)) {
                    tw_draw_status(back, s, "TRAMPWARE", l2, accent);
                    snprintf(lastL2, sizeof(lastL2), "%s", l2);
                    lastStatusS = s;
                    swap = true;
                }
            }
            layer_publish(L_STATUS, vis, (int)(24 * s), (int)(24 * s), swap);
        }

        // --- crosshair
        {
            Layer* L = &g_layer[L_CROSS];
            const bool vis = hud && m.opts[TW_TAB_HUD][H_CROSSHAIR].v;
            float s = m.opts[TW_TAB_HUD][H_SCALE].v / 100.0f;
            int w, h; tw_crosshair_size(s, &w, &h);
            bool swap = false;
            if (vis && (s != lastCrossS || lookChanged || !L->cv[L->front].px)) {
                TwCanvas* back = &L->cv[L->front ^ 1];
                if (ensure(back, w, h)) { tw_draw_crosshair(back, s, accent); lastCrossS = s; swap = true; }
            }
            layer_publish(L_CROSS, vis, W / 2 - w / 2, H / 2 - h / 2, swap);
        }

        // --- menu panel (rendered even while closed, so the first open is instant)
        {
            Layer* L = &g_layer[L_PANEL];
            float fit = fminf((W * 0.96f) / TW_PANEL_W, (H * 0.96f) / TW_PANEL_H);
            float s = m.opts[TW_TAB_VISUALS][V_SCALE].v / 100.0f;
            if (s > fit) s = fit;
            int w, h; tw_panel_size(s, &w, &h);
            bool swap = false;
            if (dirty || s != lastPanelS || !L->cv[L->front].px) {
                TwCanvas* back = &L->cv[L->front ^ 1];
                if (ensure(back, w, h)) { tw_draw_menu(back, &m, s); lastPanelS = s; swap = true; }
            }
            layer_publish(L_PANEL, open, (W - w) / 2, (H - h) / 2, swap);
        }

        // --- toast
        {
            Layer* L = &g_layer[L_TOAST];
            const bool vis = toastOn && (open || hud);
            float s = fminf(1.0f, (float)H / 1080.0f + 0.2f);
            int w = 0, h = 0;
            bool swap = false;
            if (vis) {
                tw_toast_size(s, toastText, &w, &h);
                if (strcmp(toastText, lastToast) || dirty || !L->cv[L->front].px) {
                    TwCanvas* back = &L->cv[L->front ^ 1];
                    if (ensure(back, w, h)) { tw_draw_toast(back, s, toastText); snprintf(lastToast, sizeof(lastToast), "%s", toastText); swap = true; }
                }
                w = L->cv[swap ? (L->front ^ 1) : L->front].w;   // size of the canvas that will be shown
                h = L->cv[swap ? (L->front ^ 1) : L->front].h;
            } else lastToast[0] = 0;
            layer_publish(L_TOAST, vis, (W - w) / 2, H - h - (int)(24 * s), swap);
        }
    }
    scePthreadExit(NULL);
    return NULL;
}

static void fps_tick(void) {
    const u64 t = now_ms();
    g_fpsFrames++;
    if (t - g_fpsT0 >= 500) { g_fps = g_fpsFrames * 1000.0f / (float)(t - g_fpsT0); g_fpsT0 = t; g_fpsFrames = 0; }
}

// ------------------------------------------------------------------ video out hooks
s32 sceVideoOutRegisterBuffers_hook(s32 handle, s32 startIndex, void* const* addresses, s32 bufferNum, const void* attribute) {
    s32 ret = HOOK_CONTINUE(sceVideoOutRegisterBuffers, s32 (*)(s32, s32, void* const*, s32, const void*),
                            handle, startIndex, addresses, bufferNum, attribute);
    if (g_ready && ret >= 0 && attribute && addresses && startIndex >= 0 && bufferNum > 0) {
        cvlock();
        memcpy(&g_fbAttr, attribute, sizeof(FbAttr));
        for (int i = 0; i < bufferNum; i++)
            if (startIndex + i >= 0 && startIndex + i < MAX_FB) g_fb[startIndex + i].addr = addresses[i];
        g_fbKnown = true;
        g_geo++;
        cvunlock();
        tw_log("framebuffers: %dx%d pitch=%u fmt=0x%x tiling=%d\n", g_fbAttr.width, g_fbAttr.height, g_fbAttr.pitch,
               g_fbAttr.format, g_fbAttr.tilingMode);
    }
    return ret;
}

// Classic path: the game renders, then flips. The buffer is finished, so it is safe to draw on it right here.
s32 sceVideoOutSubmitFlip_hook(s32 handle, s32 bufferIndex, u32 flipMode, s64 flipArg) {
    if (!g_gnmSeen) {
        fps_tick();
        g_prevIdx = g_curIdx;
        g_curIdx = bufferIndex;
        g_curMs = now_ms();
        if (g_ready && g_fbKnown) service_buffer(bufferIndex);
    }
    return HOOK_CONTINUE(sceVideoOutSubmitFlip, s32 (*)(s32, s32, u32, s64), handle, bufferIndex, vsync_flip(flipMode), flipArg);
}

// Gnm path: the GPU is still rendering the submitted buffer, so only note which buffers are in flight. The draw thread
// puts the overlay on them as soon as the GPU has painted over it - nothing is drawn on the game's thread.
s32 sceGnmSubmitAndFlipCommandBuffers_hook(u32 count, void* dcbGpuAddrs[], u32* dcbSizesInBytes, void* ccbGpuAddrs[],
                                           u32* ccbSizesInBytes, u32 videoOutHandle, u32 displayBufferIndex, u32 flipMode, s64 flipArg) {
    g_gnmSeen = true;
    fps_tick();
    g_prevIdx = g_curIdx;
    g_curIdx = (s32)displayBufferIndex;
    g_curMs = now_ms();
    return HOOK_CONTINUE(sceGnmSubmitAndFlipCommandBuffers,
                         s32 (*)(u32, void**, u32*, void**, u32*, u32, u32, u32, s64),
                         count, dcbGpuAddrs, dcbSizesInBytes, ccbGpuAddrs, ccbSizesInBytes, videoOutHandle, displayBufferIndex, vsync_flip(flipMode), flipArg);
}

// ------------------------------------------------------------------ plugin entry points
s32 attr_public plugin_load(s32 argc, const char* argv[]) {
    final_printf("[GoldHEN] <%s\\Ver.0x%08x> %s\n", g_pluginName, g_pluginVersion, __func__);
    boot_ver();

    struct proc_info procInfo;
    if (sys_sdk_proc_info(&procInfo) == 0) snprintf(g_titleid, sizeof(g_titleid), "%s", procInfo.titleid);

    tw_menu_init(&g_menu);
    bool hadConfig = load_config();
    sync_look();
    if (!hadConfig) save_config(&g_menu);   // create trampware.ini so the message= line can be edited

    // libScePad may not be loaded yet when the plugin starts. libSceVideoOut is always loaded by the game
    // (and loading it a second time is unnecessary), so only the pad library is loaded here.
    char module[256];
    int h = 0;
    snprintf(module, sizeof(module), "/%s/common/lib/%s", sceKernelGetFsSandboxRandomWord(), "libScePad.sprx");
    sys_dynlib_load_prx(module, &h);

    g_padReadExtPatcher = (Patcher*)malloc(sizeof(Patcher));
    g_padReadStateExtPatcher = (Patcher*)malloc(sizeof(Patcher));
    if (!g_padReadExtPatcher || !g_padReadStateExtPatcher) {
        final_printf("[trampware] out of memory\n");
        return -1;
    }
    Patcher_Construct(g_padReadExtPatcher);
    Patcher_Construct(g_padReadStateExtPatcher);

    uint8_t xor_ecx_ecx[5] = {0x31, 0xC9, 0x90, 0x90, 0x90};
    Patcher_Install_Patch(g_padReadExtPatcher, (uint64_t)scePadReadExt, xor_ecx_ecx, sizeof(xor_ecx_ecx));
    uint8_t xor_edx_edx[5] = {0x31, 0xD2, 0x90, 0x90, 0x90};
    Patcher_Install_Patch(g_padReadStateExtPatcher, (uint64_t)scePadReadStateExt, xor_edx_edx, sizeof(xor_edx_edx));

    g_ready = true;
    g_run = true;
    g_fpsT0 = now_ms();
    if (scePthreadCreate(&g_thRender, NULL, render_thread, NULL, "trampware_render") == 0 &&
        scePthreadCreate(&g_thDraw, NULL, draw_thread, NULL, "trampware_draw") == 0) { g_threadsOn = true; scePthreadSetprio(g_thDraw, 400); }
    else { g_run = false; final_printf("[trampware] could not start the overlay threads\n"); }
    HOOK32(scePadRead);
    HOOK32(scePadReadState);
    HOOK32(scePadSetVibration);
    HOOK32(sceVideoOutRegisterBuffers);
    HOOK32(sceVideoOutSubmitFlip);
    HOOK32(sceGnmSubmitAndFlipCommandBuffers);
    HOOK32(sceVideoOutSetFlipRate);

    if (OPT(TW_TAB_SYSTEM, S_NOTIFY).v) {
        char msg[128];
        if (g_loadMsg[0]) snprintf(msg, sizeof(msg), "%s", g_loadMsg);
        else snprintf(msg, sizeof(msg), "Trampware loaded - %s to open the menu",
                      OPT(TW_TAB_CONTROLS, C_TRIGGER).v ? "press L3 + R3" : "hold L1 + R1 + OPTIONS");
        NotifyStatic(TEX_ICON_SYSTEM, msg);
    }
    return 0;
}

s32 attr_public plugin_unload(s32 argc, const char* argv[]) {
    final_printf("[GoldHEN] <%s\\Ver.0x%08x> %s\n", g_pluginName, g_pluginVersion, __func__);
    g_ready = false;
    g_run = false;
    if (g_threadsOn) { scePthreadJoin(g_thRender, NULL); scePthreadJoin(g_thDraw, NULL); g_threadsOn = false; }
    UNHOOK(scePadRead);
    UNHOOK(scePadReadState);
    UNHOOK(scePadSetVibration);
    UNHOOK(sceVideoOutRegisterBuffers);
    UNHOOK(sceVideoOutSubmitFlip);
    UNHOOK(sceGnmSubmitAndFlipCommandBuffers);
    UNHOOK(sceVideoOutSetFlipRate);
    save_config(&g_menu);
    for (int i = 0; i < L_COUNT; i++) { tw_canvas_free(&g_layer[i].cv[0]); tw_canvas_free(&g_layer[i].cv[1]); free(g_layer[i].idx); free(g_layer[i].col); free(g_layer[i].nidx); free(g_layer[i].ncol); }
    return 0;
}

s32 attr_module_hidden module_start(s64 argc, const void* args) { return 0; }
s32 attr_module_hidden module_stop(s64 argc, const void* args) { return 0; }
