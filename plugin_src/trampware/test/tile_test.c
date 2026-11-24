// Host-side check of the tiled framebuffer addressing: gcc -O2 -I source test/tile_test.c source/render.c -lm -o tile_test
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tile.h"
#include "render.h"

static int check_bijection(unsigned W, unsigned H) {
    unsigned macroPitch = (W + 127) / 128, macroRows = (H + 63) / 64;
    size_t total = (size_t)macroPitch * macroRows * 8192;
    unsigned char* seen = calloc(total, 1);
    int bad = 0;
    for (unsigned y = 0; y < H; y++)
        for (unsigned x = 0; x < W; x++) {
            size_t i = tw_tiled_index(x, y, macroPitch);
            if (i >= total || seen[i]) { bad++; continue; }
            seen[i] = 1;
        }
    free(seen);
    printf("%ux%u: %s\n", W, H, bad ? "FAIL" : "ok (every pixel maps to a unique dword inside the buffer)");
    return bad;
}

int main(void) {
    int bad = check_bijection(1920, 1080) + check_bijection(1280, 720) + check_bijection(3840, 2160);

    // draw the menu into a tiled buffer, read it back through the inverse mapping and compare
    TwMenu m; tw_menu_init(&m);
    int w, h; tw_panel_size(1.0f, &w, &h);
    TwCanvas c; tw_canvas_alloc(&c, w, h); tw_draw_menu(&c, &m, 1.0f);
    const unsigned W = 1920, H = 1080, mp = (W + 127) / 128;
    uint32_t* fb = calloc((size_t)mp * 17 * 8192, 4);
    int x0 = (W - w) / 2, y0 = (H - h) / 2;
    for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) fb[tw_tiled_index(x0 + i, y0 + j, mp)] = c.px[(size_t)j * w + i];
    int diff = 0;
    for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) if (fb[tw_tiled_index(x0 + i, y0 + j, mp)] != c.px[(size_t)j * w + i]) diff++;
    printf("menu round trip: %s\n", diff ? "FAIL" : "ok");
    // what a correct display engine shows: de-tile and dump as PPM
    FILE* f = fopen("menu_preview.ppm", "wb");
    fprintf(f, "P6\n%u %u\n255\n", W, H);
    for (unsigned y = 0; y < H; y++) for (unsigned x = 0; x < W; x++) {
        uint32_t p = fb[tw_tiled_index(x, y, mp)];
        unsigned char rgb[3] = {(p >> 16) & 255, (p >> 8) & 255, p & 255};
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    return bad || diff;
}
