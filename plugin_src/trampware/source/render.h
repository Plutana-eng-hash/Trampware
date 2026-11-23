// Trampware - software renderer for the black/white menu.
// Pure C, no SDK dependencies: the same code is used by the plugin and by the host-side preview test.
#ifndef TW_RENDER_H
#define TW_RENDER_H
#include <stdint.h>

typedef struct { uint32_t* px; int w, h; } TwCanvas;   // 0xAARRGGBB, non-premultiplied

// ---- menu data model (mirrors the HTML preview) ----
enum { TW_TOGGLE, TW_SLIDER, TW_ACTION };
enum { TW_TAB_VISUALS, TW_TAB_HUD, TW_TAB_CONTROLS, TW_TAB_SYSTEM, TW_TAB_COUNT };
#define TW_MAX_OPTS 6

typedef struct {
    const char* label;
    const char* hint;
    int type;
    int v, min, max, def;
    const char* suffix;   // "%" etc
    const char* btn;      // action button caption
} TwOpt;

typedef struct {
    int tab;
    int selected;
    TwOpt opts[TW_TAB_COUNT][TW_MAX_OPTS];
    int count[TW_TAB_COUNT];
    // look & feel settings (read from the Visuals tab)
    int compact;          // 1 = compact rows
    int accent;           // 50..100
    int hints;            // show footer key hints
    const char* footer;   // footer left text
} TwMenu;

#define TW_PANEL_W 980
#define TW_PANEL_H 570

void tw_menu_init(TwMenu* m);
void tw_menu_reset(TwMenu* m);

// Canvas helpers
int  tw_canvas_alloc(TwCanvas* c, int w, int h);
void tw_canvas_free(TwCanvas* c);

// Draws the full panel at the given scale (1.0 == 980x570). Canvas must be tw_panel_size() big.
void tw_panel_size(float scale, int* w, int* h);
void tw_draw_menu(TwCanvas* c, const TwMenu* m, float scale);

// HUD pieces
void tw_status_size(float scale, int* w, int* h);
void tw_draw_status(TwCanvas* c, float scale, const char* line1, const char* line2, int accent);
void tw_crosshair_size(float scale, int* w, int* h);
void tw_draw_crosshair(TwCanvas* c, float scale, int accent);

// Toast bubble (like the HTML toast)
void tw_toast_size(float scale, const char* text, int* w, int* h);
void tw_draw_toast(TwCanvas* c, float scale, const char* text);

#endif
