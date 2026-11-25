# Trampware

Black / white in-game overlay menu for GoldHEN (`trampware.prx`).

- **Open:** press `L3 + R3` together (Controls tab can switch this to `L1 + R1 + OPTIONS`). **Close:** `O` (Circle) from anywhere, or `L3 + R3` again
- **Categories:** D-Pad up/down moves between categories, `X` (or right) opens one, `O` closes the menu
- **Options:** D-Pad up/down moves between options, left/right adjusts sliders and toggles, `X` enters / clicks the option, `TRIANGLE` goes back to the categories, `O` closes the menu
- **L1 / R1** switch tabs from anywhere, `SQUARE` resets the selected option to its default
- **Settings file:** `/data/GoldHEN/trampware.ini` (written when the menu closes)

Tabs: Visuals (opacity, scale, accent, compact layout), HUD (status panel with FPS, crosshair, HUD scale),
Controls (block game input while open, allow/deny vibration, navigation speed, button hints), System (logging, safe mode, reset, about).

The menu is software-rendered (`source/render.c`, no SDK dependencies) on a background render thread and composited
onto the game's video-out buffers by a background draw thread. Nothing heavy runs on the game or controller threads, so
L3 + R3 opens/closes instantly. The draw thread stamps the menu / watermark (always fully opaque) onto every frame buffer a few hundred times a
second, so whichever frame is on screen always has them and they no longer flicker. Only **linear** A8R8G8B8 / A8B8G8R8 buffers are drawn while
*Draw on tiled games* is enabled.

Install: copy `trampware.prx` to `/data/GoldHEN/plugins/` and add it to `/data/GoldHEN/plugins.ini`.

## Tiled games
Most games use tiled framebuffers. The plugin draws on them with the PS4 2D-thin display tiling (`source/tile.h`):
128x64 px macro tiles, 8 pipes / 16 banks, 8x8 micro tiles. The bit layout was decoded from a photo of the old
probe pattern taken on real hardware. **System > Draw on tiled games** (on by default) switches this on; turn it off
if a game ever shows glitched blocks. `test/tile_test.c` checks the address mapping on the host.
The plugin shows one notification when it loads and no other notifications.
