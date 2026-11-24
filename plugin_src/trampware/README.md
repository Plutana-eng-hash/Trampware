# Trampware

Black / white in-game overlay menu for GoldHEN (`trampware.prx`).

- **Open:** press `L3 + R3` together (Controls tab can switch this to `L1 + R1 + OPTIONS`). **Close:** `O` (Circle) or `L3 + R3` again
- **Navigate:** D-Pad up/down = select, left/right = adjust, `X` = toggle / activate, `L1`/`R1` = switch tab, `Square` = reset option, `O` = close
- **Settings file:** `/data/GoldHEN/trampware.ini` (written when the menu closes)

Tabs: Visuals (opacity, scale, accent, compact layout), HUD (status panel with FPS, crosshair, HUD scale),
Controls (block game input while open, allow/deny vibration, navigation speed, button hints), System (logging, safe mode, reset, about).

The menu is software-rendered (`source/render.c`, no SDK dependencies) and composited onto the game's video-out
buffer from the `sceVideoOutSubmitFlip` hook. Only **linear** A8R8G8B8 / A8B8G8R8 buffers are drawn while
*Safe mode* is on (default); games using tiled framebuffers will not show the overlay.

Install: copy `trampware.prx` to `/data/GoldHEN/plugins/` and add it to `/data/GoldHEN/plugins.ini`.

## Tiled games (experimental)
Games that use tiled framebuffers show no overlay while *Safe mode* is on (the menu is sent as notifications instead).
Turn **System > Safe mode** off (or set `3.1=0` in `/data/GoldHEN/trampware.ini`) to test drawing on tiled buffers.
Expect a scrambled menu until the exact macro-tile layout of the game is mapped - a photo of the screen is enough to fix it.
