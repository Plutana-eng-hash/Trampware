# Trampware

Black / white in-game overlay menu for GoldHEN (`trampware.prx`).

- **Open:** press `L3 + R3` together and hold or press once; it opens one time and each new press toggles it. (Controls tab can switch this to `L1 + R1 + OPTIONS`). **Close:** `O` (Circle) from anywhere, or press `L3 + R3` again
- **Categories:** D-Pad up/down moves between categories, `X` (or right) opens one, `O` closes the menu
- **Options:** D-Pad up/down moves between options, left/right adjusts sliders and toggles, `X` enters / clicks the option, `TRIANGLE` goes back to the categories, `O` closes the menu
- **L1 / R1** switch tabs from anywhere, `SQUARE` resets the selected option to its default
- **Settings file:** `/data/GoldHEN/trampware.ini` (written when the menu closes)

Tabs: Visuals (opacity, scale, accent, compact layout), HUD (status panel with FPS, crosshair, HUD scale, V-Sync override and V-Sync on/off),
Controls (block game input while open, allow/deny vibration, navigation speed, button hints), System (logging, reset, about).

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

## V-Sync
HUD > **Override V-Sync** turns the override on. **V-Sync** then forces the flip mode to on or off for every frame the game presents. With the override off, the game's own choice is used. The two flip-mode numbers are `FLIP_MODE_VSYNC_ON` and `FLIP_MODE_VSYNC_OFF` at the top of `source/main.c`; check them on console and change them there if V-Sync does not switch.


## Frame rate
Trampware no longer includes an FPS unlocker. Use the separate `fliprate_remover` plugin in this repo for that.

## Log folder (for bug reports)
Trampware writes everything it does to `/data/trampware/trampware.log` on the PS4 (the folder is created automatically).
Each line is written as it happens, so the last line before a crash shows where it stopped. The log rotates to
`trampware.old.log` at 4 MB. It includes the game's title ID, process details, firmware/SDK version, every option value,
menu opens and closes, controller combo presses, framebuffer registrations, and a status line every 5 seconds.
Send that file when reporting a crash.

## Disabling Trampware for a game
Put a title ID on its own line in `/data/trampware/disabled_titles.txt`. Trampware then installs nothing for that game.
The title ID of the running game is written at the top of `trampware.log`.
