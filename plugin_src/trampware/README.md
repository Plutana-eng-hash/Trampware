# Trampware

Black / white in-game overlay menu for GoldHEN (`trampware.prx`).

- **Open:** press `L3 + R3` together (Controls tab can switch this to `L1 + R1 + OPTIONS`). **Close:** `O` (Circle) or `L3 + R3` again
- **Navigate:** D-Pad up/down = select, left/right = adjust, `X` = toggle / activate, `L1`/`R1` = switch tab, `Square` = reset option, `O` = close
- **Settings file:** `/data/GoldHEN/trampware.ini` (written when the menu closes)

Tabs: Visuals (opacity, scale, accent, compact layout), HUD (status panel with FPS, crosshair, HUD scale),
Controls (block game input while open, allow/deny vibration, navigation speed, button hints), System (logging, safe mode, reset, about).

The menu is software-rendered (`source/render.c`, no SDK dependencies) and composited onto the game's video-out
buffer from the `sceVideoOutSubmitFlip` hook. Only **linear** A8R8G8B8 / A8B8G8R8 buffers are drawn while
*Draw on tiled games* is enabled.

Install: copy `trampware.prx` to `/data/GoldHEN/plugins/` and add it to `/data/GoldHEN/plugins.ini`.

## Tiled games
Most games use tiled framebuffers. The plugin draws on them with the PS4 2D-thin display tiling (`source/tile.h`):
128x64 px macro tiles, 8 pipes / 16 banks, 8x8 micro tiles. The bit layout was decoded from a photo of the old
probe pattern taken on real hardware. **System > Draw on tiled games** (on by default) switches this on; turn it off
if a game ever shows glitched blocks. `test/tile_test.c` checks the address mapping on the host.
The plugin shows one notification when it loads and no other notifications.
