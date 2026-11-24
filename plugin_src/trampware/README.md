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

## Tiled games (experimental)
Most games use tiled framebuffers. **System > Draw on tiled games** (on by default) draws the menu on them using an
8x8 micro-tile layout. If the menu looks like shuffled blocks, the game also uses macro tiling - send a photo so the layout can be mapped.
The plugin shows one notification when it loads and no other notifications.
