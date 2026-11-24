// Trampware - black/white in-game overlay menu for GoldHEN.
//
// Open / close:  press L3 + R3 (toggle).  CIRCLE also closes.  (Controls tab can switch to L1 + R1 + OPTIONS)
// Navigate:      D-Pad UP/DOWN select, LEFT/RIGHT adjust, CROSS toggle/activate, L1/R1 switch tab, CIRCLE close
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
attr_public u32 g_pluginVersion = 0x00000100;  // 1.00

#define CONFIG_PATH GOLDHEN_PATH "/trampware.ini"
#define COMBO_L3R3 (SCE_PAD_BUTTON_L3 | SCE_PAD_BUTTON_R3)
#define COMBO_ALT  (SCE_PAD_BUTTON_L1 | SCE_PAD_BUTTON_R1 | SCE_PAD_BUTTON_OPTIONS)
#define OPEN_COMBO open_combo()
#define NAV_MASK (SCE_PAD_BUTTON_UP | SCE_PAD_BUTTON_DOWN | SCE_PAD_BUTTON_LEFT | SCE_PAD_BUTTON_RIGHT | \
                  SCE_PAD_BUTTON_CROSS | SCE_PAD_BUTTON_CIRCLE | SCE_PAD_BUTTON_L1 | SCE_PAD_BUTTON_R1)

// Option indices (see kDefaults in render.c)
enum { V_ENABLED, V_COMPACT, V_OPACITY, V_SCALE, V_ACCENT };
enum { H_ENABLED, H_CROSSHAIR, H_STATUS, H_SCALE };
enum { C_BLOCK, C_VIBRATION, C_SPEED, C_HINTS, C_TRIGGER };
enum { S_LOGGING, S_TILED, S_NOTIFY, S_RESET, S_ABOUT };

HOOK_INIT(scePadRead);
HOOK_INIT(scePadReadState);
HOOK_INIT(scePadSetVibration);
HOOK_INIT(sceVideoOutRegisterBuffers);
HOOK_INIT(sceVideoOutSubmitFlip);
HOOK_INIT(sceGnmSubmitAndFlipCommandBuffers);

// Most PS4 games present through the graphics driver, not sceVideoOutSubmitFlip (same hook as frame_logger).
s32 sceGnmSubmitAndFlipCommandBuffers(u32 count, void* dcbGpuAddrs[], u32* dcbSizesInBytes, void* ccbGpuAddrs[],
                                      u32* ccbSizesInBytes, u32 videoOutHandle, u32 displayBufferIndex, u32 flipMode, s64 flipArg);
static volatile bool g_gnmSeen;   // once the Gnm flip path is seen, the VideoOut hook stops drawing
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
#define MAX_MARKS 4
typedef struct {
    TwCanvas cv[2];
    int front;
    bool visible;
    int x, y, alpha;
    int nm, mx[MAX_MARKS], my[MAX_MARKS];   // opaque sample pixels (canvas coordinates) used to detect "overlay was wiped"
} Layer;
static Layer g_layer[L_COUNT];
static volatile u32 g_ver = 1;              // bumped whenever what has to be on screen changes

// Per framebuffer: which version of the overlay it holds + the exact pixels we wrote at the sample points
#define MAX_BMARK 16
typedef struct { u32 ver; u64 t; int n; int x[MAX_BMARK], y[MAX_BMARK]; u32 val[MAX_BMARK]; } BufState;
static BufState g_bs[MAX_FB];
static volatile s32 g_curIdx = -1;          // buffer submitted last (still being rendered / about to be shown)

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
    if (open) OPT(TW_TAB_VISUALS, V_ENABLED).v = 1;
    else g_saveReq = true;   // written by the render thread, so closing is instant
    g_dirty = true;
    tw_log("menu %s\n", open ? "opened" : "closed");
}

// Handles one pad sample. Returns true if the sample should be hidden from the game.
static u32 g_prevBtn, g_swallow;
static u64 g_nextRepeat;

static bool menu_input(u32 btn) {
    u32 pressed = btn & ~g_prevBtn;
    g_prevBtn = btn;

    // Open/close: press L3 + R3 together (or L1+R1+OPTIONS if switched in Controls). CIRCLE (O) also closes.
    const u32 oc = open_combo();
    const bool combo = (btn & oc) == oc && (pressed & oc);
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

    if (p & SCE_PAD_BUTTON_L1) { g_menu.tab = (g_menu.tab + TW_TAB_COUNT - 1) % TW_TAB_COUNT; g_menu.selected = 0; g_dirty = true; }
    if (p & SCE_PAD_BUTTON_R1) { g_menu.tab = (g_menu.tab + 1) % TW_TAB_COUNT; g_menu.selected = 0; g_dirty = true; }
    if (p & SCE_PAD_BUTTON_CIRCLE) set_open(false);
    if (p & SCE_PAD_BUTTON_CROSS) activate(cur);
    if ((p & SCE_PAD_BUTTON_SQUARE) && cur->type != TW_ACTION) {   // SQUARE = reset the selected option
        cur->v = cur->def; sync_look(); g_dirty = true; toast("Reset to default");
    }

    // d-pad with key repeat (navigation speed 1..10)
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

// ------------------------------------------------------------------ compositing
// Pixel index inside the framebuffer. Linear: row-major. Tiled: PS4 2D-thin display tiling, see tile.h.
static bool g_tiledDraw;
static unsigned g_macroPitch = 15;
static inline size_t fb_index(int x, int y, int pitch) {
    if (!g_tiledDraw) return (size_t)y * pitch + x;
    return tw_tiled_index((unsigned)x, (unsigned)y, g_macroPitch);
}

static void blit(const TwCanvas* c, void* fbp, int fbPitch, int fbW, int fbH, int x0, int y0, int globalA, bool swapRB) {
    for (int j = 0; j < c->h; j++) {
        int y = y0 + j;
        if (y < 0 || y >= fbH) continue;
        const u32* src = c->px + (size_t)j * c->w;
        for (int i = 0; i < c->w; i++) {
            u32 s = src[i];
            if (!(s >> 24)) continue;   // fully transparent
            int x = x0 + i;
            if (x < 0 || x >= fbW) continue;
            u32 a = ((s >> 24) * globalA) / 255;
            if (!a) continue;
            u32 sr = (s >> 16) & 255, sg = (s >> 8) & 255, sb = s & 255;
            if (swapRB) { u32 t = sr; sr = sb; sb = t; }
            u32* dp = (u32*)fbp + fb_index(x, y, fbPitch);
            if (a >= 255) { *dp = 0xFF000000u | (sr << 16) | (sg << 8) | sb; continue; }
            u32 d = *dp;
            u32 dr = (d >> 16) & 255, dg = (d >> 8) & 255, db = d & 255;
            dr += ((int)(sr - dr) * (int)a) / 255;
            dg += ((int)(sg - dg) * (int)a) / 255;
            db += ((int)(sb - db) * (int)a) / 255;
            *dp = 0xFF000000u | (dr << 16) | (dg << 8) | db;
        }
    }
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
// Pick a few fully opaque pixels of the front canvas. If the game later repaints the buffer, these pixels no
// longer hold what we wrote there, which is how the draw thread knows the overlay has to be put back.
static void pick_marks(Layer* L) {
    const TwCanvas* c = &L->cv[L->front];
    static const int fx[5] = {15, 85, 15, 85, 50}, fy[5] = {15, 15, 85, 85, 50};
    L->nm = 0;
    if (!c->px) return;
    for (int k = 0; k < 5 && L->nm < MAX_MARKS; k++) {
        int x = c->w * fx[k] / 100, y = c->h * fy[k] / 100;
        if ((c->px[(size_t)y * c->w + x] >> 24) == 255) { L->mx[L->nm] = x; L->my[L->nm] = y; L->nm++; }
    }
}

// Make `li` show the given state. swap = the back canvas was just rendered and becomes the front one.
static void layer_publish(int li, bool vis, int x, int y, int alpha, bool swap) {
    Layer* L = &g_layer[li];
    if (!swap && L->visible == vis && L->x == x && L->y == y && L->alpha == alpha) return;
    cvlock();
    if (swap) { L->front ^= 1; pick_marks(L); }
    L->visible = vis && L->cv[L->front].px != NULL;
    L->x = x; L->y = y; L->alpha = alpha;
    g_ver++;
    cvunlock();
}

// ---- drawing onto one framebuffer (caller holds g_cvLock and has called fb_supported())
static inline u32 fb_get(const void* fbp, int pitch, int x, int y) { return ((const u32*)fbp)[fb_index(x, y, pitch)]; }

static bool buffer_ok(int idx) {
    const BufState* s = &g_bs[idx];
    if (s->ver != g_ver) return false;
    const int pitch = g_fbAttr.pitch ? (int)g_fbAttr.pitch : (int)g_fbAttr.width;
    for (int k = 0; k < s->n; k++)
        if (fb_get(g_fb[idx].addr, pitch, s->x[k], s->y[k]) != s->val[k]) return false;
    return true;
}

static void draw_buffer(int idx) {
    const int W = (int)g_fbAttr.width, H = (int)g_fbAttr.height;
    const int pitch = g_fbAttr.pitch ? (int)g_fbAttr.pitch : W;
    const bool swap = (g_fbAttr.format == FB_FMT_A8B8G8R8);
    void* fbp = g_fb[idx].addr;
    BufState* s = &g_bs[idx];
    s->ver = g_ver;
    s->n = 0;
    for (int li = 0; li < L_COUNT; li++) {
        const Layer* L = &g_layer[li];
        const TwCanvas* c = &L->cv[L->front];
        if (!L->visible || !c->px) continue;
        blit(c, fbp, pitch, W, H, L->x, L->y, L->alpha, swap);
        for (int k = 0; k < L->nm && s->n < MAX_BMARK; k++) {
            int x = L->x + L->mx[k], y = L->y + L->my[k];
            if (x < 0 || y < 0 || x >= W || y >= H) continue;
            s->x[s->n] = x; s->y[s->n] = y; s->val[s->n] = fb_get(fbp, pitch, x, y);
            s->n++;
        }
    }
}

// Put the overlay on buffer idx if it is missing (never drawn, wiped by the game, or out of date).
static void service_buffer(int idx) {
    if (idx < 0 || idx >= MAX_FB || !g_fb[idx].addr || !fb_supported()) return;
    cvlock();
    if (!buffer_ok(idx)) {
        BufState* s = &g_bs[idx];
        const u64 t = now_ms();
        // a repaint of the same version may be redrawn quickly, but not in a tight loop
        if (s->ver != g_ver || t - s->t >= 3) { draw_buffer(idx); s->t = t; }
    }
    cvunlock();
}

// Draw thread: keeps the overlay on the frame that is on screen and the one about to be shown. The game renders a
// buffer from scratch every few frames, so whenever it wipes ours this puts the overlay back within about a millisecond.
static void* draw_thread(void* arg) {
    (void)arg;
    while (g_run) {
        sceKernelUsleep(1000);
        if (!g_ready || !g_fbKnown) continue;
        s32 cur = g_curIdx, prev = g_prevIdx;
        service_buffer(prev);   // on screen right now
        if (cur != prev) service_buffer(cur);
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
            layer_publish(L_STATUS, vis, (int)(24 * s), (int)(24 * s), 255, swap);
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
            layer_publish(L_CROSS, vis, W / 2 - w / 2, H / 2 - h / 2, 255, swap);
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
            layer_publish(L_PANEL, open, (W - w) / 2, (H - h) / 2, m.opts[TW_TAB_VISUALS][V_OPACITY].v * 255 / 100, swap);
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
            layer_publish(L_TOAST, vis, (W - w) / 2, H - h - (int)(24 * s), 255, swap);
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
            if (startIndex + i >= 0 && startIndex + i < MAX_FB) { g_fb[startIndex + i].addr = addresses[i]; memset(&g_bs[startIndex + i], 0, sizeof(BufState)); }
        g_fbKnown = true;
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
        if (g_ready && g_fbKnown) service_buffer(bufferIndex);
    }
    return HOOK_CONTINUE(sceVideoOutSubmitFlip, s32 (*)(s32, s32, u32, s64), handle, bufferIndex, flipMode, flipArg);
}

// Gnm path: the GPU is still rendering the submitted buffer, so only note which buffers are in flight. The draw thread
// puts the overlay on them as soon as the GPU has painted over it - nothing is drawn on the game's thread.
s32 sceGnmSubmitAndFlipCommandBuffers_hook(u32 count, void* dcbGpuAddrs[], u32* dcbSizesInBytes, void* ccbGpuAddrs[],
                                           u32* ccbSizesInBytes, u32 videoOutHandle, u32 displayBufferIndex, u32 flipMode, s64 flipArg) {
    g_gnmSeen = true;
    fps_tick();
    g_prevIdx = g_curIdx;
    g_curIdx = (s32)displayBufferIndex;
    return HOOK_CONTINUE(sceGnmSubmitAndFlipCommandBuffers,
                         s32 (*)(u32, void**, u32*, void**, u32*, u32, u32, u32, s64),
                         count, dcbGpuAddrs, dcbSizesInBytes, ccbGpuAddrs, ccbSizesInBytes, videoOutHandle, displayBufferIndex, flipMode, flipArg);
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
        scePthreadCreate(&g_thDraw, NULL, draw_thread, NULL, "trampware_draw") == 0) g_threadsOn = true;
    else { g_run = false; final_printf("[trampware] could not start the overlay threads\n"); }
    HOOK32(scePadRead);
    HOOK32(scePadReadState);
    HOOK32(scePadSetVibration);
    HOOK32(sceVideoOutRegisterBuffers);
    HOOK32(sceVideoOutSubmitFlip);
    HOOK32(sceGnmSubmitAndFlipCommandBuffers);

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
    save_config(&g_menu);
    for (int i = 0; i < L_COUNT; i++) { tw_canvas_free(&g_layer[i].cv[0]); tw_canvas_free(&g_layer[i].cv[1]); }
    return 0;
}

s32 attr_module_hidden module_start(s64 argc, const void* args) { return 0; }
s32 attr_module_hidden module_stop(s64 argc, const void* args) { return 0; }
