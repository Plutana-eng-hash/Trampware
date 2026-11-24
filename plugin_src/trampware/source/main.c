// Trampware - black/white in-game overlay menu for GoldHEN.
//
// Open / close:  L1 + R1 + OPTIONS   (hold the three together)
// Navigate:      D-Pad UP/DOWN select, LEFT/RIGHT adjust, CROSS toggle/activate, L1/R1 switch tab, CIRCLE close
//
// The menu is software-rendered (render.c) and composited onto the game's video-out buffer from the
// sceVideoOutSubmitFlip hook. Settings are stored in /data/GoldHEN/trampware.ini.

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
#include "plugin_common.h"
#include "pad.h"
#include "render.h"

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
static bool g_dirty = true;
static volatile int g_lock;
static char g_titleid[16] = "";

// toast
static char g_toast[64];
static u64 g_toastUntil;

// canvases
static TwCanvas g_panel, g_status, g_cross, g_toastCv;
static float g_panelScale, g_statusScale, g_crossScale;
static char g_statusL2[48], g_toastShown[64];

// fps
static u64 g_fpsT0;
static int g_fpsFrames;
static float g_fps;

// ------------------------------------------------------------------ helpers
static u32 open_combo(void);   // defined after the options are available
static inline void lock(void) { while (__atomic_exchange_n(&g_lock, 1, __ATOMIC_ACQUIRE)) {} }
static inline void unlock(void) { __atomic_store_n(&g_lock, 0, __ATOMIC_RELEASE); }

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
static void save_config(void) {
    FILE* f = fopen(CONFIG_PATH, "w");
    if (!f) return;
    for (int t = 0; t < TW_TAB_COUNT; t++)
        for (int i = 0; i < g_menu.count[t]; i++)
            if (g_menu.opts[t][i].type != TW_ACTION) fprintf(f, "%d.%d=%d\n", t, i, g_menu.opts[t][i].v);
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
    else save_config();
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
// Pixel index inside the framebuffer. Linear: row-major. Tiled (experimental): 8x8 micro tiles, row-major tile order,
// pixel order inside a tile = x0 x1 y0 x2 y1 y2 (GCN display micro-tiling, 32bpp). Macro/bank swizzle is NOT applied yet,
// so on 2D-tiled games the menu appears as shuffled 8x8 blocks - which shows the real macro layout.
static bool g_tiledDraw;
static inline size_t fb_index(int x, int y, int pitch) {
    if (!g_tiledDraw) return (size_t)y * pitch + x;
    unsigned m = (x & 1) | (((x >> 1) & 1) << 1) | ((y & 1) << 2) | (((x >> 2) & 1) << 3) | (((y >> 1) & 1) << 4) | (((y >> 2) & 1) << 5);
    return ((size_t)(y >> 3) * (size_t)(pitch >> 3) + (size_t)(x >> 3)) * 64 + m;
}

static void blit(const TwCanvas* c, void* fbp, int fbPitch, int fbW, int fbH, int x0, int y0, int globalA, bool swapRB) {
    for (int j = 0; j < c->h; j++) {
        int y = y0 + j;
        if (y < 0 || y >= fbH) continue;
        const u32* src = c->px + (size_t)j * c->w;
        for (int i = 0; i < c->w; i++) {
            int x = x0 + i;
            if (x < 0 || x >= fbW) continue;
            u32 s = src[i];
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

static void draw_overlay(void* fbp) {
    const int W = (int)g_fbAttr.width, H = (int)g_fbAttr.height;
    const int pitch = g_fbAttr.pitch ? (int)g_fbAttr.pitch : W;
    const bool swap = (g_fbAttr.format == FB_FMT_A8B8G8R8);
    const u64 t = now_ms();

    // fps
    g_fpsFrames++;
    if (t - g_fpsT0 >= 500) { g_fps = g_fpsFrames * 1000.0f / (float)(t - g_fpsT0); g_fpsT0 = t; g_fpsFrames = 0; }

    lock();
    const bool open = g_open && OPT(TW_TAB_VISUALS, V_ENABLED).v;
    const bool hud = OPT(TW_TAB_HUD, H_ENABLED).v;
    const int accent = OPT(TW_TAB_VISUALS, V_ACCENT).v;
    bool toastOn = g_toast[0] && t < g_toastUntil;
    if (!toastOn && g_toast[0]) { g_toast[0] = 0; g_dirty = true; }

    if (hud && OPT(TW_TAB_HUD, H_STATUS).v) {
        float s = OPT(TW_TAB_HUD, H_SCALE).v / 100.0f * ((float)H / 1080.0f < 0.67f ? 0.8f : 1.0f);
        char l2[48]; snprintf(l2, sizeof(l2), "%s   %d FPS", g_titleid[0] ? g_titleid : "GAME", (int)(g_fps + 0.5f));
        int w, h; tw_status_size(s, &w, &h);
        if (ensure(&g_status, w, h) && (strcmp(l2, g_statusL2) || s != g_statusScale || g_dirty)) {
            strncpy(g_statusL2, l2, sizeof(g_statusL2) - 1);
            g_statusScale = s;
            tw_draw_status(&g_status, s, "TRAMPWARE", l2, accent);
        }
        if (g_status.px) blit(&g_status, fbp, pitch, W, H, (int)(24 * s), (int)(24 * s), 255, swap);
    }
    if (hud && OPT(TW_TAB_HUD, H_CROSSHAIR).v) {
        float s = OPT(TW_TAB_HUD, H_SCALE).v / 100.0f;
        int w, h; tw_crosshair_size(s, &w, &h);
        if (ensure(&g_cross, w, h) && (s != g_crossScale || g_dirty)) { g_crossScale = s; tw_draw_crosshair(&g_cross, s, accent); }
        if (g_cross.px) blit(&g_cross, fbp, pitch, W, H, W / 2 - w / 2, H / 2 - h / 2, 255, swap);
    }
    if (open) {
        float fit = fminf((W * 0.96f) / TW_PANEL_W, (H * 0.96f) / TW_PANEL_H);
        float s = OPT(TW_TAB_VISUALS, V_SCALE).v / 100.0f;
        if (s > fit) s = fit;
        int w, h; tw_panel_size(s, &w, &h);
        if (ensure(&g_panel, w, h) && (g_dirty || s != g_panelScale)) {
            g_panelScale = s;
            sync_look();
            tw_draw_menu(&g_panel, &g_menu, s);
        }
        if (g_panel.px) blit(&g_panel, fbp, pitch, W, H, (W - w) / 2, (H - h) / 2, OPT(TW_TAB_VISUALS, V_OPACITY).v * 255 / 100, swap);
    }
    if (toastOn && (open || hud)) {
        float s = fminf(1.0f, (float)H / 1080.0f + 0.2f);
        int w, h; tw_toast_size(s, g_toast, &w, &h);
        if (ensure(&g_toastCv, w, h) && (strcmp(g_toast, g_toastShown) || g_dirty)) {
            strncpy(g_toastShown, g_toast, sizeof(g_toastShown) - 1);
            tw_draw_toast(&g_toastCv, s, g_toast);
        }
        if (g_toastCv.px) blit(&g_toastCv, fbp, pitch, W, H, (W - w) / 2, H - h - (int)(24 * s), 255, swap);
    }
    g_dirty = false;
    unlock();
}

// ------------------------------------------------------------------ video out hooks
s32 sceVideoOutRegisterBuffers_hook(s32 handle, s32 startIndex, void* const* addresses, s32 bufferNum, const void* attribute) {
    s32 ret = HOOK_CONTINUE(sceVideoOutRegisterBuffers, s32 (*)(s32, s32, void* const*, s32, const void*),
                            handle, startIndex, addresses, bufferNum, attribute);
    if (g_ready && ret >= 0 && attribute && addresses && startIndex >= 0 && bufferNum > 0) {
        memcpy(&g_fbAttr, attribute, sizeof(FbAttr));
        for (int i = 0; i < bufferNum; i++)
            if (startIndex + i >= 0 && startIndex + i < MAX_FB) g_fb[startIndex + i].addr = addresses[i];
        g_fbKnown = true;
        tw_log("framebuffers: %dx%d pitch=%u fmt=0x%x tiling=%d\n", g_fbAttr.width, g_fbAttr.height, g_fbAttr.pitch,
               g_fbAttr.format, g_fbAttr.tilingMode);
    }
    return ret;
}

s32 sceVideoOutSubmitFlip_hook(s32 handle, s32 bufferIndex, u32 flipMode, s64 flipArg) {
    if (g_ready && g_fbKnown && bufferIndex >= 0 && bufferIndex < MAX_FB && g_fb[bufferIndex].addr) {
        const bool fmtOk = (g_fbAttr.format == FB_FMT_A8R8G8B8 || g_fbAttr.format == FB_FMT_A8B8G8R8);
        const bool linear = (g_fbAttr.tilingMode == FB_TILING_LINEAR);
        const bool tiledOn = OPT(TW_TAB_SYSTEM, S_TILED).v != 0;
        g_tiledDraw = !linear;
        const bool dimsOk = linear || ((g_fbAttr.pitch % 8) == 0 && (g_fbAttr.height % 8) == 0 && g_fbAttr.pitch >= g_fbAttr.width);
        if (fmtOk && dimsOk && (linear || tiledOn)) {
            g_overlayOk = true;
            draw_overlay(g_fb[bufferIndex].addr);
        } else {
            g_overlayOk = false;
        }
    }
    return HOOK_CONTINUE(sceVideoOutSubmitFlip, s32 (*)(s32, s32, u32, s64), handle, bufferIndex, flipMode, flipArg);
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
    if (!hadConfig) save_config();   // create trampware.ini so the message= line can be edited

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
    HOOK32(scePadRead);
    HOOK32(scePadReadState);
    HOOK32(scePadSetVibration);
    HOOK32(sceVideoOutRegisterBuffers);
    HOOK32(sceVideoOutSubmitFlip);

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
    UNHOOK(scePadRead);
    UNHOOK(scePadReadState);
    UNHOOK(scePadSetVibration);
    UNHOOK(sceVideoOutRegisterBuffers);
    UNHOOK(sceVideoOutSubmitFlip);
    save_config();
    tw_canvas_free(&g_panel); tw_canvas_free(&g_status); tw_canvas_free(&g_cross); tw_canvas_free(&g_toastCv);
    return 0;
}

s32 attr_module_hidden module_start(s64 argc, const void* args) { return 0; }
s32 attr_module_hidden module_stop(s64 argc, const void* args) { return 0; }
