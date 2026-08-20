# Cursor Unbound

An SKSE plugin that decouples the Skyrim menu cursor from the game frame rate.

## The problem

"Sluggish menu cursor" is actually two separate problems that INI tweaks conflate:

1. **Speed scaling.** Skyrim applies per-frame multipliers to mouse deltas without
   normalising by frame time, so cursor travel per inch of mousepad changes with fps.
   `fMouseCursorSpeed` and friends just rescale a broken curve.
2. **Update rate.** The menu cursor is a Scaleform sprite drawn inside the render frame.
   It can never update more often than the game renders. At 45 fps that is a 22 ms sample
   interval with visible stepping, and no sensitivity value fixes it.

Every INI guide addresses only #1. That is why tweaking plateaus.

## What Cursor Unbound does

**For #2 — update rate.** It hides the Scaleform cursor and hands drawing to the Windows
hardware cursor. Windows composites the hardware cursor on the GPU's dedicated cursor
plane at monitor refresh rate, completely independently of what the application is doing.
That is the only mechanism on Windows that gives true OS-level pointer smoothness, and it
is unaffected by the game's frame rate.

**For #1 — speed scaling.** It hooks `CursorMenu::ProcessMouseMove` and overwrites the
game's cursor position with the OS cursor position mapped into the game's coordinate
space, rather than letting it integrate an fps-scaled delta. Scaleform hit-testing, hover
states and clicks then follow the OS cursor exactly.

Menu logic still runs at game fps, so highlight repaint latency is unchanged. The pointer
glide — the thing that actually reads as sluggish — is not.

## Requirements

- Skyrim Special Edition or Anniversary Edition (built and tested against runtime
  **1.6.1170**; the DLL is address-library based and covers SE and AE generally)
- SKSE64
- Address Library for SKSE Plugins

**Skyrim VR is not supported.** In VR builds CommonLibSSE strips `MenuEventHandler` off
`CursorMenu`, which removes the virtual this plugin hooks. VR would need a different
approach entirely.

## Installation

Install as a normal mod. The layout is:

```
SKSE/Plugins/CursorUnbound.dll
SKSE/Plugins/CursorUnbound.ini
SKSE/Plugins/CursorUnbound/cursor.png
```

**Use borderless windowed rather than exclusive fullscreen.** In exclusive fullscreen the
hardware cursor may not composite, which is the single most likely reason for "I installed
it and now I have no cursor". SSE Display Tweaks provides borderless windowed. On Windows
11 with fullscreen optimisations enabled, exclusive fullscreen is usually composited
anyway and works — but if the cursor vanishes, this is the first thing to change.

## The cursor art

The bundled `cursor.png` is an ESO-style arrow (25x32) with its hotspot at the tip.
`cursor_48.png` is the same art larger, and `cursor_gold*.png` are procedurally generated
parchment-toned alternatives.

The plugin **cannot read cursor art out of a `.swf`**, so if you use a cursor replacer
(ESO Style Cursor, Vel'dun UI, and similar all ship a `cursormenu.swf`), the bundled arrow
will not match it. To match your replacer, export its art to a PNG and either drop it in
as `SKSE/Plugins/CursorUnbound/cursor.png` or point `CursorFile` at it.

Anything WIC can decode works: `.png`, `.bmp`, `.tif`, `.jpg`, `.gif`. `.cur` and `.ani`
also work and are handed straight to Windows — an `.ani` will animate at OS rate, driven
by the compositor rather than the game.

`tools/make_cursor.py` regenerates the default art if you want to tweak the shape; it is
stdlib-only Python, no Pillow required. `--style=gold` produces a warm parchment arrow for
UI overhauls, `--size=48` a larger one.

### Making art for a patch

`tools/convert_cursor.py` turns an arbitrary image into cursor-ready RGBA: it crops to
content, keys out a flat background when the source has no alpha, and resizes.

```bash
python tools/convert_cursor.py exported.png cursor.png --size 32 --key none
```

If you are exporting from a `cursormenu.swf` with a decompiler, use the **shape** export,
not the frame export. Frame export renders the entire 640×480 stage, background and all,
with the cursor as a speck in the middle — it looks fine in an image viewer and is useless
as cursor art.

Do not redistribute art extracted from another mod unless its permissions allow it. A
patch that ships instructions rather than artwork is always safe.

## Configuration

See the comments in `CursorUnbound.ini`. The settings most worth knowing:

| Setting | Purpose |
| --- | --- |
| `UseHardwareCursor` | `false` keeps the game drawing its own cursor. You still get frame-rate independent sensitivity, but the pointer stays frame-locked. Useful for A/B comparison. |
| `HideMethod` | Which mechanism suppresses the game's cursor sprite. `render` is the default and the only one that reliably works; `rootalpha`, `viewport` and `all` are fallbacks. |
| `AbsolutePositioning` | The speed-scaling fix, independent of the hardware cursor swap. |
| `NeutralizeGameDelta` | Stops the game integrating movement on top of the absolute position. Disabling it reintroduces overshoot jitter. |
| `CoordinateSpace` | Leave on `auto` unless the cursor is visually offset from where clicks land. |
| `LogCursorRange` | Diagnostic. Logs the coordinate range the game itself produces. |
| `BlockGameCursorHide` | Stops the game re-hiding the OS cursor. Disable if it fights another mod. |

Logs go to `Documents\My Games\Skyrim Special Edition\SKSE\CursorUnbound.log`.

## Troubleshooting

**No cursor at all in menus.** Switch to borderless windowed. If that fixes it, exclusive
fullscreen was not compositing the hardware cursor.

**Two cursors.** One is the hardware cursor (this plugin); the other is the game's
Scaleform sprite failing to hide. `HideMethod = render` is the default and suppresses the
draw call outright, so this should not happen — if it does, try `viewport`, then
`rootalpha`. Note that `setvisible`, `rootvisible` and `rootalpha` can all report success
while the cursor is still drawn, because the game re-shows it every frame.

**Cursor is visually offset from where clicks register.** The coordinate space guess is
wrong. Calibrate: set `AbsolutePositioning = false` and `LogCursorRange = true` — you must
disable the positioning first or you will just read your own values back. Open a menu,
move into all four corners, read the logged `cursorPos` min/max, put those maxima into
`SpanX`/`SpanY`, then re-enable `AbsolutePositioning`. Try `CoordinateSpace = client`
first — it is the simpler fix.

**Cursor flickers on menu transitions.** Make sure `BlockGameCursorHide = true`. If the
log says `USER32!ShowCursor is not in the game's import table`, the plugin is falling back
to `WM_SETCURSOR`, which is more flicker-prone.

**No cursor until you alt-tab out and back.** This was a bug up to 1.0.0 and is fixed. If
it recurs, check the log for `syncTimer=false` on the `Window procedure hooked` line - the
plugin could not start its message-queue timer and is back to needing mouse input before it
notices a menu. `Activated (... showCursorCount=N)` should report `N >= 0`; a negative N
means something is still driving the OS display counter down behind the plugin.

**Conflicts.** Mods that draw their own pointer (ImGui-based overlays) or manage cursor
visibility can fight this. Skyrim Souls RE changes which menus are open and is worth
testing early.

## Building

Requires Visual Studio 2022 with the C++ desktop toolset, and vcpkg.

```bash
git clone --recurse-submodules https://github.com/<you>/CursorUnbound.git
```

If you already cloned without `--recurse-submodules`:

```bash
git submodule update --init --recursive
```

vcpkg is found via `VCPKG_ROOT` or the `-VcpkgRoot` argument; CMake is taken from PATH, or
from the copy bundled with Visual Studio.

```bash
powershell -ExecutionPolicy Bypass -File build.ps1
```

That configures, builds Release and stages into `dist/`. To also install into a mod folder
for testing, add `-ModDir`:

```bash
powershell -ExecutionPolicy Bypass -File build.ps1 -ModDir "E:\MO2 Mods\Cursor Unbound"
```

Deploying never overwrites an existing `CursorUnbound.ini` or cursor art - updated defaults
land beside them as `.new`. Pass `-Clean` after changing CMake options.

`package.ps1` produces the release archive in `release/`. It takes the version from
`project(VERSION ...)` in `CMakeLists.txt`, which is the only place the version is written
down - `main.cpp` reads it from there too, via compile definitions, for both the SKSE
plugin record and the log banner.

CommonLibSSE-NG is a submodule at `extern/CommonLibSSE-NG`.

## Releasing

`.github/workflows/build.yml` builds on every push and pull request, and on a `v*` tag it
also packages the archive and attaches it to a draft GitHub release:

```bash
git tag v1.0.1 && git push origin v1.0.1
```

The tag is checked against `CMakeLists.txt` and the build fails on a mismatch, so a tag
cannot ship an archive labelled with a different version. The release is created as a
draft - review it and publish by hand.

Nexus is not automated. Nexus Mods has no public upload API, so the archive still has to
be uploaded there manually.

## How it works

| Piece | Mechanism |
| --- | --- |
| Position | Vtable detour on `CursorMenu::ProcessMouseMove` (`VTABLE_CursorMenu[1]`, index 4). Writes `MenuCursor::cursorPosX/Y` from `GetCursorPos` **before** calling the original, with the event delta zeroed for the duration of that call and restored afterwards. |
| Drawing | `SetVisible(false)` plus `_root._visible = false` and `_root._alpha = 0` on the Cursor Menu movie; the Win32 hardware cursor draws instead. |
| Visibility | IAT patch on `USER32!ShowCursor` to swallow the game's hide calls, plus a 15 ms `WM_TIMER` in a subclassed window procedure that re-asserts the display counter and the cursor image. The timer is what makes this work without mouse input - `WM_SETCURSOR` only arrives once the pointer moves, so on its own it cannot bring the cursor up on a menu that just opened. |
| Art | WIC decode to 32bpp PBGRA, `CreateIconIndirect` with an all-zero AND mask so the alpha channel drives blending. |
| Gamepad | Vtable detour on `ProcessThumbstick` (same vtable, index 3). A stick past a deadzone hands the cursor back to the game; any real mouse delta takes it back. |

The draw suppression holds a `GPtr` strong reference to the movie it is suppressing, not
just a raw pointer. Without it the movie could be freed and a different movie allocated at
the same address, and the instance check would then blank out the wrong movie. The raw
atomic pointer exists alongside it only because `Display` runs on the render thread, where
copying a `GPtr` would not be atomic.

The `ShowCursor` hook returns `-1` for swallowed hide calls rather than `0`. The game's
visibility helper is a `do { count = ShowCursor(false); } while (count >= 0);` loop, so
reporting "still visible" from a swallowed call would hang it.

The hook ordering is load-bearing. The original `ProcessMouseMove` is what pushes the
cursor position into Scaleform via `GFxMovieView::NotifyMouseState`, so writing the
position *after* it returns leaves the drawn cursor and the hit-test following the game's
fps-scaled integration while `MenuCursor` holds the absolute value. The two disagree every
frame, which reads as jitter. Writing before — with the delta zeroed so the original does
not integrate on top — is what makes Scaleform agree with the OS position.
