#include "render.h"
#include "font.h"
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------- basics
int tw_canvas_alloc(TwCanvas* c, int w, int h) {
    c->px = (uint32_t*)calloc((size_t)w * h, 4);
    c->w = w; c->h = h;
    return c->px != NULL;
}
void tw_canvas_free(TwCanvas* c) { free(c->px); c->px = NULL; c->w = c->h = 0; }

static inline float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }

// src-over of colour `col` (alpha in top byte) scaled by `cov` (0..1) onto canvas pixel
static inline void blend(TwCanvas* c, int x, int y, uint32_t col, float cov) {
    if ((unsigned)x >= (unsigned)c->w || (unsigned)y >= (unsigned)c->h) return;
    float sa = ((col >> 24) / 255.0f) * cov;
    if (sa <= 0.0f) return;
    uint32_t d = c->px[(size_t)y * c->w + x];
    float da = (d >> 24) / 255.0f;
    float oa = sa + da * (1.0f - sa);
    float r = (((col >> 16) & 255) * sa + ((d >> 16) & 255) * da * (1.0f - sa)) / oa;
    float g = (((col >> 8) & 255) * sa + ((d >> 8) & 255) * da * (1.0f - sa)) / oa;
    float b = (((col) & 255) * sa + ((d) & 255) * da * (1.0f - sa)) / oa;
    c->px[(size_t)y * c->w + x] = ((uint32_t)(oa * 255.0f + 0.5f) << 24) | ((uint32_t)(r + 0.5f) << 16) |
                                  ((uint32_t)(g + 0.5f) << 8) | (uint32_t)(b + 0.5f);
}

#define RGB(r, g, b) (0xFF000000u | ((r) << 16) | ((g) << 8) | (b))
#define GRAY(v) RGB(v, v, v)

// signed distance to a rounded box centred at (cx,cy) with half size (hx,hy) and radius r
static inline float sd_rbox(float px, float py, float cx, float cy, float hx, float hy, float r) {
    float qx = fabsf(px - cx) - (hx - r), qy = fabsf(py - cy) - (hy - r);
    float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
    float inner = qx > qy ? qx : qy; if (inner > 0) inner = 0;
    return sqrtf(ox * ox + oy * oy) + inner - r;
}

// Rounded rect with optional border (fill==0 -> no fill, border==0 -> no border). Coordinates are final pixels.
static void rrect(TwCanvas* c, float x, float y, float w, float h, float r, uint32_t fill, uint32_t border, float bw) {
    if (r < 0.01f && fill && !border) {   // fast path: plain rectangle, area coverage, no sqrt
        int ya = (int)floorf(y), yb = (int)ceilf(y + h), xa = (int)floorf(x), xb = (int)ceilf(x + w);
        if (ya < 0) ya = 0; if (xa < 0) xa = 0; if (yb > c->h) yb = c->h; if (xb > c->w) xb = c->w;
        for (int j = ya; j < yb; j++) {
            float cy = fminf(j + 1.0f, y + h) - fmaxf((float)j, y);
            for (int i = xa; i < xb; i++) {
                float cx = fminf(i + 1.0f, x + w) - fmaxf((float)i, x);
                float cov = cx * cy;
                if (cov >= 0.999f && (fill >> 24) == 255) c->px[(size_t)j * c->w + i] = fill;
                else if (cov > 0) blend(c, i, j, fill, cov);
            }
        }
        return;
    }
    int x0 = (int)floorf(x) - 1, y0 = (int)floorf(y) - 1, x1 = (int)ceilf(x + w) + 1, y1 = (int)ceilf(y + h) + 1;
    float cx = x + w * 0.5f, cy = y + h * 0.5f;
    for (int j = y0; j < y1; j++)
        for (int i = x0; i < x1; i++) {
            float d = sd_rbox(i + 0.5f, j + 0.5f, cx, cy, w * 0.5f, h * 0.5f, r);
            float cov = clampf(0.5f - d, 0, 1);
            if (cov <= 0) continue;
            if (fill) blend(c, i, j, fill, cov);
            if (border && bw > 0) {
                float inner = clampf(d + bw + 0.5f, 0, 1);   // 1 inside the border band
                blend(c, i, j, border, cov * inner);
            }
        }
}
static void hline(TwCanvas* c, float x, float y, float w, float th, uint32_t col) { rrect(c, x, y, w, th, 0, col, 0, 0); }

static void disc(TwCanvas* c, float cx, float cy, float r, uint32_t col, float ring) {
    for (int j = (int)(cy - r) - 1; j <= (int)(cy + r) + 1; j++)
        for (int i = (int)(cx - r) - 1; i <= (int)(cx + r) + 1; i++) {
            float d = hypotf(i + 0.5f - cx, j + 0.5f - cy) - r;
            float cov = clampf(0.5f - d, 0, 1);
            if (ring > 0) cov *= clampf(d + ring + 0.5f, 0, 1);
            blend(c, i, j, col, cov);
        }
}

// ---------------------------------------------------------------- text
static const TwGlyph* gl(int bold, int ch) { return (bold ? tw_bold_g : tw_reg_g) + (ch - 32); }
static const unsigned char* gp(int bold) { return bold ? tw_bold_px : tw_reg_px; }

static float text_w(const char* s, float px, int bold, float spacing) {
    float k = px / TW_FONT_MASTER, w = 0;
    for (; *s; s++) {
        int ch = (unsigned char)*s; if (ch < 32 || ch > 126) ch = '?';
        w += gl(bold, ch)->adv16 / 16.0f * k + spacing;
    }
    return w;
}

// cy = vertical centre of the text line; align 0 left / 1 right / 2 centre
static void text(TwCanvas* c, float x, float cy, float px, int bold, float spacing, uint32_t col,
                 const char* s, int align, float shear) {
    float k = px / TW_FONT_MASTER;
    float tw = text_w(s, px, bold, spacing) - spacing;
    if (align == 1) x -= tw; else if (align == 2) x -= tw * 0.5f;
    float base = cy + px * 0.365f;
    int n = (int)ceilf(1.0f / k); if (n < 1) n = 1; if (n > 4) n = 4;
    for (; *s; s++) {
        int ch = (unsigned char)*s; if (ch < 32 || ch > 126) ch = '?';
        const TwGlyph* g = gl(bold, ch);
        const unsigned char* pix = gp(bold) + g->off;
        if (g->w > 0) {
            float gx = x + g->l * k, gy = base + g->t * k;
            int ix0 = (int)floorf(gx) - 1, ix1 = (int)ceilf(gx + g->w * k) + 1;
            int iy0 = (int)floorf(gy) - 1, iy1 = (int)ceilf(gy + g->h * k) + 1;
            for (int j = iy0; j < iy1; j++) {
                float sh = shear * (base - (j + 0.5f));
                for (int i = ix0; i < ix1; i++) {
                    float acc = 0;
                    for (int b = 0; b < n; b++)
                        for (int a = 0; a < n; a++) {
                            float sx = (i + (a + 0.5f) / n - sh - gx) / k, sy = (j + (b + 0.5f) / n - gy) / k;
                            int ux = (int)floorf(sx), uy = (int)floorf(sy);
                            if (ux >= 0 && uy >= 0 && ux < g->w && uy < g->h) acc += pix[uy * g->w + ux];
                        }
                    acc /= (255.0f * n * n);
                    if (acc > 0) blend(c, i, j, col, acc);
                }
            }
        }
        x += g->adv16 / 16.0f * k + spacing;
    }
}

// ---------------------------------------------------------------- data
static const TwOpt kDefaults[TW_TAB_COUNT][TW_MAX_OPTS] = {
    /* Visuals */ {
        {"Menu enabled", "Show the interface", TW_TOGGLE, 1, 0, 1, 1, "", ""},
        {"Compact layout", "Reduce spacing between elements", TW_TOGGLE, 0, 0, 1, 0, "", ""},
        {"UI opacity", "Panel transparency", TW_SLIDER, 92, 30, 100, 92, "%", ""},
        {"Interface scale", "Overall size of the menu", TW_SLIDER, 100, 75, 125, 100, "%", ""},
        {"Accent brightness", "White highlight intensity", TW_SLIDER, 100, 50, 100, 100, "%", ""},
    },
    /* HUD */ {
        {"HUD elements", "Display the HUD layer", TW_TOGGLE, 1, 0, 1, 1, "", ""},
        {"Crosshair", "Show center reticle", TW_TOGGLE, 0, 0, 1, 0, "", ""},
        {"Status panel", "Show FPS and title information", TW_TOGGLE, 1, 0, 1, 1, "", ""},
        {"HUD scale", "Size of HUD elements", TW_SLIDER, 100, 60, 140, 100, "%", ""},
    },
    /* Controls */ {
        {"Controller input", "Block game input while the menu is open", TW_TOGGLE, 1, 0, 1, 1, "", ""},
        {"Vibration", "Allow the game to vibrate the controller", TW_TOGGLE, 1, 0, 1, 1, "", ""},
        {"Navigation speed", "Repeat rate for directional input", TW_SLIDER, 5, 1, 10, 5, "", ""},
        {"Button hints", "Display navigation hints", TW_TOGGLE, 1, 0, 1, 1, "", ""},
    },
    /* System */ {
        {"Logging", "Write plugin messages to the kernel log", TW_TOGGLE, 0, 0, 1, 0, "", ""},
        {"Safe mode", "Only draw on linear framebuffers", TW_TOGGLE, 1, 0, 1, 1, "", ""},
        {"Reset settings", "Restore Trampware to defaults", TW_ACTION, 0, 0, 0, 0, "", "Reset"},
        {"About Trampware", "Black / white menu", TW_ACTION, 0, 0, 0, 0, "", "Info"},
    },
};
static const int kCounts[TW_TAB_COUNT] = {5, 4, 4, 4};

void tw_menu_reset(TwMenu* m) {
    int tab = m->tab, sel = m->selected;
    memcpy(m->opts, kDefaults, sizeof(kDefaults));
    memcpy(m->count, kCounts, sizeof(kCounts));
    m->tab = tab; m->selected = sel;
    m->compact = 0; m->accent = 100; m->hints = 1;
    if (!m->footer) m->footer = "Trampware for GoldHEN";
}
void tw_menu_init(TwMenu* m) { memset(m, 0, sizeof(*m)); m->footer = "Trampware for GoldHEN"; tw_menu_reset(m); }

// ---------------------------------------------------------------- icons (the HTML uses unicode glyphs; drawn as shapes here)
static void icon(TwCanvas* c, int which, float cx, float cy, float s, uint32_t col) {
    float th = fmaxf(1.0f, s * 1.3f);
    switch (which) {
    case 0: { // diamond with centre dot
        for (int j = (int)(cy - 9 * s); j <= (int)(cy + 9 * s); j++)
            for (int i = (int)(cx - 9 * s); i <= (int)(cx + 9 * s); i++) {
                float d = (fabsf(i + .5f - cx) + fabsf(j + .5f - cy)) - 7.5f * s;
                float ring = clampf(0.5f - fabsf(d) + th * 0.5f - 0.5f, 0, 1);
                float dot = clampf(0.5f - ((fabsf(i + .5f - cx) + fabsf(j + .5f - cy)) - 3.0f * s), 0, 1);
                blend(c, i, j, col, fmaxf(ring, dot));
            }
    } break;
    case 1: // square with inner block
        rrect(c, cx - 7 * s, cy - 7 * s, 14 * s, 14 * s, 1.5f * s, 0, col, th);
        rrect(c, cx - 3.5f * s, cy - 3.5f * s, 7 * s, 7 * s, 0.5f * s, col, 0, 0);
        break;
    case 2: // command-style: small square in the middle, four corner rings
        rrect(c, cx - 3.5f * s, cy - 3.5f * s, 7 * s, 7 * s, 0, 0, col, th);
        for (int q = 0; q < 4; q++) disc(c, cx + ((q & 1) ? 5.5f : -5.5f) * s, cy + ((q & 2) ? 5.5f : -5.5f) * s, 2.8f * s, col, th);
        break;
    case 3: // gear
        disc(c, cx, cy, 5.2f * s, col, th * 1.4f);
        for (int t = 0; t < 8; t++) {
            float a = t * 0.7853982f;
            disc(c, cx + cosf(a) * 7.2f * s, cy + sinf(a) * 7.2f * s, 1.7f * s, col, 0);
        }
        break;
    }
}

// ---------------------------------------------------------------- layout
void tw_panel_size(float scale, int* w, int* h) { *w = (int)ceilf(TW_PANEL_W * scale); *h = (int)ceilf(TW_PANEL_H * scale); }

static uint32_t gray_a(int v, int a) { return ((uint32_t)a << 24) | (v << 16) | (v << 8) | v; }

void tw_draw_menu(TwCanvas* c, const TwMenu* m, float sc) {
#define P(v) floorf((v) * sc + 0.5f)
#define F(v) ((v) * sc)
    memset(c->px, 0, (size_t)c->w * c->h * 4);
    int acc = 127 + (int)(128.0f * (m->accent - 50) / 50.0f);        // accent gray
    if (acc > 255) acc = 255; if (acc < 120) acc = 120;
    uint32_t ACC = GRAY(acc);

    // panel + sidebar
    rrect(c, 0, 0, c->w, c->h, 0, RGB(16, 16, 16), 0, 0);
    rrect(c, 0, 0, P(220), c->h, 0, RGB(10, 10, 10), 0, 0);
    rrect(c, P(219), 0, P(1), c->h, 0, RGB(39, 39, 39), 0, 0);

    // brand
    {
        float lx = P(25), ly = P(29), ls = P(36);
        float cxm = lx + ls * 0.5f;
        // skewed (-7deg) box: draw 4 edges by shearing each scanline
        float sh = 0.1228f;
        for (int j = 0; j < (int)ls; j++) {
            float off = (ls * 0.5f - (j + 0.5f)) * sh;
            float t = fmaxf(1.0f, P(1));
            bool edge = (j < t) || (j >= ls - t);
            if (edge) rrect(c, lx + off, ly + j, ls, 1, 0, RGB(238, 238, 238), 0, 0);
            else { rrect(c, lx + off, ly + j, t, 1, 0, RGB(238, 238, 238), 0, 0); rrect(c, lx + off + ls - t, ly + j, t, 1, 0, RGB(238, 238, 238), 0, 0); }
        }
        text(c, cxm, ly + ls * 0.5f, F(18), 1, 0, RGB(244, 244, 244), "T", 2, sh);
        float nx = P(73), ny = P(47);
        text(c, nx, ny, F(17), 1, F(2), RGB(244, 244, 244), "TRAMP", 0, 0);
        float tw = text_w("TRAMP", F(17), 1, F(2));
        text(c, nx + tw, ny, F(17), 1, F(2), RGB(119, 119, 119), "WARE", 0, 0);
    }

    // nav
    static const char* tabs[TW_TAB_COUNT] = {"Visuals", "HUD", "Controls", "System"};
    for (int i = 0; i < TW_TAB_COUNT; i++) {
        float x = P(17), y = P(92 + i * 51.5f), w = P(186), h = P(47);
        bool act = (i == m->tab);
        uint32_t fg = act ? RGB(8, 8, 8) : RGB(136, 136, 136);
        if (act) rrect(c, x, y, w, h, F(4), (acc >= 250) ? RGB(240, 240, 240) : GRAY((acc * 240) / 255), 0, 0);
        icon(c, i, x + F(12 + 10), y + h * 0.5f, sc, fg);
        text(c, x + F(12 + 20 + 11), y + h * 0.5f, F(14), act, 0, fg, tabs[i], 0, 0);
    }

    // sidebar bottom
    hline(c, P(17), P(471), P(186), P(1), RGB(32, 32, 32));
    disc(c, P(25 + 3), P(471 + 16 + 8), F(3), RGB(221, 221, 221), 0);
    text(c, P(25 + 14), P(471 + 16 + 8), F(11), 0, F(1), RGB(170, 170, 170), "MENU", 0, 0);
    text(c, P(25), P(471 + 16 + 8 + 24), F(11), 0, F(1), RGB(96, 96, 96), "UI BUILD 0.1.0", 0, 0);
    text(c, P(25), P(471 + 16 + 8 + 24 + 16.5f), F(11), 0, F(1), RGB(96, 96, 96), "BLACK / WHITE INTERFACE", 0, 0);

    // header
    static const char* crumbs[TW_TAB_COUNT] = {"VISUALS", "HUD", "CONTROLS", "SYSTEM"};
    static const char* titles[TW_TAB_COUNT] = {"Visual settings", "HUD settings", "Control settings", "System settings"};
    float mx = P(252), mr = P(948);
    text(c, mx, P(35), F(11), 0, F(2), RGB(112, 112, 112), "CONFIGURATION / ", 0, 0);
    text(c, mx + text_w("CONFIGURATION / ", F(11), 0, F(2)), P(35), F(11), 0, F(2), RGB(112, 112, 112), crumbs[m->tab], 0, 0);
    text(c, mx, P(64), F(22), 1, F(0.3f), RGB(244, 244, 244), titles[m->tab], 0, 0);
    {
        float tw = text_w("TRAMPWARE ", F(11), 0, F(1)) + F(9);
        float bw = tw + P(18) + 2, bh = P(30.5f), bx = mr - bw, by = P(53.75f) - bh * 0.5f;
        rrect(c, bx, by, bw, bh, F(4), 0, RGB(41, 41, 41), 1);
        text(c, bx + P(9) + 1, by + bh * 0.5f, F(11), 0, F(1), RGB(119, 119, 119), "TRAMPWARE ", 0, 0);
        float ox = bx + P(9) + 1 + text_w("TRAMPWARE ", F(11), 0, F(1));
        disc(c, ox + F(4.5f), by + bh * 0.5f, F(3.2f), RGB(221, 221, 221), 0);
    }
    hline(c, mx, P(98.5f), mr - mx, P(1), RGB(39, 39, 39));

    // section head
    text(c, mx, P(129.5f), F(11), 0, F(1.8f), RGB(119, 119, 119), "OPTIONS", 0, 0);
    hline(c, mx, P(148), mr - mx, P(1), RGB(36, 36, 36));

    // rows
    float rowh = m->compact ? 46.0f : 60.0f;
    for (int i = 0; i < m->count[m->tab]; i++) {
        const TwOpt* o = &m->opts[m->tab][i];
        float ry = P(157 + i * (rowh + 2)), rh = P(rowh);
        bool sel = (i == m->selected);
        if (sel) {
            rrect(c, mx, ry, mr - mx, rh, F(4), RGB(20, 20, 20), RGB(58, 58, 58), 1);
            rrect(c, mx, ry + F(10), F(2), rh - F(20), 0, ACC, 0, 0);
        }
        float lx = mx + P(13), cy = ry + rh * 0.5f;
        float lab = m->compact ? -7.5f : -9.5f, hin = m->compact ? 9.5f : 10.5f;
        text(c, lx, cy + F(lab), F(13), 1, 0, RGB(244, 244, 244), o->label, 0, 0);
        text(c, lx, cy + F(hin), F(11), 0, 0, RGB(105, 105, 105), o->hint, 0, 0);
        float rx = mr - P(13);
        if (o->type == TW_TOGGLE) {
            float w = P(42), h = P(23), x = rx - w, y = cy - h * 0.5f;
            bool on = o->v;
            rrect(c, x, y, w, h, F(3), on ? ACC : RGB(36, 36, 36), on ? ACC : RGB(69, 69, 69), 1);
            float k = P(15);
            rrect(c, on ? x + P(22) : x + P(3), y + P(3), k, k, F(2), on ? RGB(17, 17, 17) : RGB(119, 119, 119), 0, 0);
        } else if (o->type == TW_SLIDER) {
            float vw = P(36);
            char buf[16]; int n = 0, v = o->v; char t[8]; int tn = 0;
            if (v == 0) t[tn++] = '0'; while (v > 0) { t[tn++] = '0' + v % 10; v /= 10; }
            while (tn) buf[n++] = t[--tn];
            for (const char* q = o->suffix; *q; q++) buf[n++] = *q; buf[n] = 0;
            text(c, rx, cy, F(12), 0, 0, RGB(170, 170, 170), buf, 1, 0);
            float sw = P(140), sx = rx - vw - P(14) - sw;
            float frac = (float)(o->v - o->min) / (float)(o->max - o->min);
            float th = P(4);
            rrect(c, sx, cy - th * 0.5f, sw, th, th * 0.5f, RGB(58, 58, 58), 0, 0);
            rrect(c, sx, cy - th * 0.5f, sw * frac, th, th * 0.5f, ACC, 0, 0);
            disc(c, sx + F(7) + (sw - F(14)) * frac, cy, F(7), ACC, 0);
        } else {
            float tw = text_w(o->btn, F(12), 0, 0);
            float w = tw + P(22) + 2, h = P(34), x = rx - w, y = cy - h * 0.5f;
            rrect(c, x, y, w, h, F(3), RGB(23, 23, 23), sel ? ACC : RGB(51, 51, 51), 1);
            text(c, x + w * 0.5f, cy, F(12), 0, 0, sel ? RGB(255, 255, 255) : RGB(221, 221, 221), o->btn, 2, 0);
        }
    }

    // footer
    float fy = P(157 + 5 * 62 - 2 + 22);
    hline(c, mx, fy, mr - mx, P(1), RGB(36, 36, 36));
    float fcy = fy + P(13) + F(8.25f);
    text(c, mx, fcy, F(11), 0, 0, RGB(94, 94, 94), m->footer, 0, 0);
    if (m->hints) {
        // right aligned: [UP/DOWN] select  [LEFT/RIGHT] adjust  [X] toggle  [L1/R1] tab
        static const char* keys[4] = {"UP/DOWN", "LEFT/RIGHT", "X", "L1/R1"};
        static const char* what[4] = {" select  ", " adjust  ", " toggle  ", " tab"};
        float total = 0;
        for (int k = 0; k < 4; k++) total += text_w(keys[k], F(11), 0, 0) + P(12) + 2 + text_w(what[k], F(11), 0, 0);
        float x = mr - total;
        for (int k = 0; k < 4; k++) {
            float kw = text_w(keys[k], F(11), 0, 0) + P(12) + 2;
            rrect(c, x, fcy - F(9.5f), kw, F(19), F(3), 0, RGB(51, 51, 51), 1);
            text(c, x + kw * 0.5f, fcy, F(11), 0, 0, RGB(170, 170, 170), keys[k], 2, 0);
            x += kw;
            text(c, x, fcy, F(11), 0, 0, RGB(94, 94, 94), what[k], 0, 0);
            x += text_w(what[k], F(11), 0, 0);
        }
    }

    // rounded outer border / mask (radius 8, border #292929)
    float r = F(8);
    for (int j = 0; j < c->h; j++)
        for (int i = 0; i < c->w; i++) {
            if (i > r + 1 && j > r + 1 && i < c->w - r - 1 && j < c->h - r - 1 && i > 1.5f && j > 1.5f && i < c->w - 2.5f && j < c->h - 2.5f) continue;
            float d = sd_rbox(i + 0.5f, j + 0.5f, c->w * 0.5f, c->h * 0.5f, c->w * 0.5f, c->h * 0.5f, r);
            float cov = clampf(0.5f - d, 0, 1);
            uint32_t p = c->px[(size_t)j * c->w + i];
            if (d > -1.0f) { // border band
                float t = clampf(d + 1.5f, 0, 1);
                uint32_t bc = RGB(41, 41, 41);
                int rr = ((p >> 16) & 255), gg = ((p >> 8) & 255), bb = (p & 255);
                rr += (int)(((bc >> 16 & 255) - rr) * t); gg += (int)(((bc >> 8 & 255) - gg) * t); bb += (int)(((bc & 255) - bb) * t);
                p = 0xFF000000u | (rr << 16) | (gg << 8) | bb;
            }
            p = (p & 0x00FFFFFFu) | ((uint32_t)(cov * 255.0f + 0.5f) << 24);
            c->px[(size_t)j * c->w + i] = p;
        }
#undef P
#undef F
}

// ---------------------------------------------------------------- HUD
void tw_status_size(float s, int* w, int* h) { *w = (int)ceilf(190 * s); *h = (int)ceilf(48 * s); }
void tw_draw_status(TwCanvas* c, float s, const char* l1, const char* l2, int accent) {
    memset(c->px, 0, (size_t)c->w * c->h * 4);
    int acc = 127 + (int)(128.0f * (accent - 50) / 50.0f); if (acc > 255) acc = 255;
    rrect(c, 0, 0, c->w, c->h, 5 * s, RGB(16, 16, 16), RGB(41, 41, 41), 1);
    rrect(c, 0, 8 * s, 2 * s, c->h - 16 * s, 0, GRAY(acc), 0, 0);
    text(c, 14 * s, 15 * s, 12 * s, 1, 1.0f * s, RGB(244, 244, 244), l1, 0, 0);
    text(c, 14 * s, 33 * s, 11 * s, 0, 0, RGB(150, 150, 150), l2, 0, 0);
}
void tw_crosshair_size(float s, int* w, int* h) { *w = *h = (int)ceilf(33 * s); }
void tw_draw_crosshair(TwCanvas* c, float s, int accent) {
    memset(c->px, 0, (size_t)c->w * c->h * 4);
    int acc = 127 + (int)(128.0f * (accent - 50) / 50.0f); if (acc > 255) acc = 255;
    float m = c->w * 0.5f, t = fmaxf(1.0f, floorf(1.5f * s)), g = 4 * s, l = 12 * s;
    uint32_t col = GRAY(acc), sh = gray_a(0, 160);
    for (int pass = 0; pass < 2; pass++) {
        float o = pass == 0 ? 1.0f : 0.0f; uint32_t cc = pass == 0 ? sh : col;
        rrect(c, m - t / 2 + o, m - g - l + o, t, l, 0, cc, 0, 0);
        rrect(c, m - t / 2 + o, m + g + o, t, l, 0, cc, 0, 0);
        rrect(c, m - g - l + o, m - t / 2 + o, l, t, 0, cc, 0, 0);
        rrect(c, m + g + o, m - t / 2 + o, l, t, 0, cc, 0, 0);
    }
    disc(c, m, m, fmaxf(1.0f, s), col, 0);
}

// ---------------------------------------------------------------- toast
void tw_toast_size(float s, const char* t, int* w, int* h) {
    *w = (int)ceilf(text_w(t, 13 * s, 1, 0) + 28 * s); *h = (int)ceilf(34 * s);
}
void tw_draw_toast(TwCanvas* c, float s, const char* t) {
    memset(c->px, 0, (size_t)c->w * c->h * 4);
    rrect(c, 0, 0, c->w, c->h, 4 * s, RGB(238, 238, 238), 0, 0);
    text(c, c->w * 0.5f, c->h * 0.5f, 13 * s, 1, 0, RGB(17, 17, 17), t, 2, 0);
}
