// Trampware - PS4 display framebuffer tiling (2D thin, 32bpp, 8 pipes / 16 banks, 1x1 bank, 128x64 px macro tile).
//
// Decoded from a photo of the on-screen tile probe taken on real hardware:
//   * a framebuffer is a grid of 128x64 px macro tiles (16x8 micro tiles), stored row-major,
//     each macro tile is 128 micro tiles = 32 KiB;
//   * inside a macro tile the micro tile number is  (bank << 3) | pipe  (pipe = low 3 bits);
//   * a micro tile is 8x8 px = 64 dwords, pixel order inside it = x0 x1 y0 x2 y1 y2.
// With tx = x/8 and ty = y/8 (micro-tile coordinates, bN = bit N):
//   pipe0 = tx0^tx1^ty0      bank0 = tx3^ty3
//   pipe1 = tx1^ty1          bank1 = tx4^ty2^ty3
//   pipe2 = tx2^ty2          bank2 = tx5^ty1
//                            bank3 = tx6^ty0
#ifndef TW_TILE_H
#define TW_TILE_H
#include <stddef.h>

// Index (in 32-bit pixels) of pixel (x,y) inside a tiled framebuffer. macroPitch = ceil(pitchPixels / 128).
static inline size_t tw_tiled_index(unsigned x, unsigned y, unsigned macroPitch) {
    const unsigned tx = x >> 3, ty = y >> 3;
    const unsigned m = (x & 1) | (((x >> 1) & 1) << 1) | ((y & 1) << 2) | (((x >> 2) & 1) << 3) |
                       (((y >> 1) & 1) << 4) | (((y >> 2) & 1) << 5);
    const unsigned pipe = ((tx ^ (tx >> 1) ^ ty) & 1u) | ((((tx >> 1) ^ (ty >> 1)) & 1u) << 1) |
                          ((((tx >> 2) ^ (ty >> 2)) & 1u) << 2);
    const unsigned bank = (((tx >> 3) ^ (ty >> 3)) & 1u) | ((((tx >> 4) ^ (ty >> 2) ^ (ty >> 3)) & 1u) << 1) |
                          ((((tx >> 5) ^ (ty >> 1)) & 1u) << 2) | ((((tx >> 6) ^ ty) & 1u) << 3);
    const size_t macro = (size_t)(ty >> 3) * macroPitch + (tx >> 4);
    return (((macro << 7) | (size_t)((bank << 3) | pipe)) << 6) | m;
}
#endif
