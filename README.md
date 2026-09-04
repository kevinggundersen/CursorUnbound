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

- Skyrim Special Edition or Anniversary Edition (developed against runtime **1.6.1170**;
  the DLL is address-library based and covers SE and AE generally, including **1.7.99**
  and **1.7.104**)
- SKSE64 — 2.3.0 or later on 1.7.99, 2.3.1 or later on 1.7.104
- Address Library for SKSE Plugins — on 1.7.99 you need a build that contains
  `versionlib-1-7-99-0.bin`; on 1.7.104, one that contains `versionlib-1-7-104-0.bin`
  (Address Library 13.0 or later)

**On 1.7.99 and later** the game inserted two new virtuals into `MenuEventHandler`,
shifting the two slots this plugin hooks down by two. The plugin detects the runtime and
picks the right slots; the log line `Hooked CursorMenu::ProcessMouseMove (vfunc 0x6)`
reports which pair it resolved to (`0x6`/`0x5` on 1.7.99 and 1.7.104, `0x4`/`0x3` below
it). If the cursor does nothing on a future runtime, that line is the first thing to check.

**1.7.104 note.** SKSE 2.3.1 added a plugin-header flag declaring compatibility with the
address-library encoding used since 1.7.99. Builds from 1.0.9 onward declare it (via
CommonLibSSE-NG 7.2.0); a 1.0.8 DLL still loads on 2.3.1 only because SKSE waives the
check for DLLs built after May 2025.

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
| `HookAllModules` | Also intercepts `ShowCursor` calls from other DLLs (SSEDisplayTweaks, other SKSE plugins), not just the game executable. Turn off if another cursor mod stops working. |
| `EnforceHiddenWhenInactive` | Keeps re-hiding the OS cursor while no menu wants it, rather than hiding it once on menu close. Turn off if a mod that wants a pointer during gameplay cannot show one. |
| `SuppressPrismaCursor` | Blanks PrismaUI's own cursor sprite so it does not double up with the hardware one. |
| `SuppressPartySheetCursor` | The same, for Skyrim Party Sheet. |
| `TrackPartySheetPanels` | Shows the hardware cursor while a Party Sheet panel is open. See below. |
| `SuppressGridInventoryCursor` | The same, for Grid Inventory's ImGui pointer. Falls back to standing our own cursor down if its signature goes stale. |

Logs go to `Documents\My Games\Skyrim Special Edition\SKSE\CursorUnbound.log`.

## Other UI frameworks

Mods that draw their own pointer instead of using the game's cursor menu need explicit
handling, because their sprite is drawn inside the game frame and so trails the hardware
cursor. Three are handled:

**PrismaUI** — its cursor sprite is blanked while this plugin is drawing one
(`SuppressPrismaCursor`). Prisma views drive the cursor menu, so nothing else is needed.

**[Skyrim Party Sheet](https://www.nexusmods.com/skyrimspecialedition/mods/167538)** — its
panels are an ImGui overlay in the game's present hook, so they are invisible to every way
this plugin normally detects that a pointer is wanted. Party Sheet broadcasts its panel
state over SKSE messaging, and this plugin listens for it (`TrackPartySheetPanels`): while
one of its interactive panels is open, the hardware cursor comes up and Party Sheet's own
pointer is suppressed (`SuppressPartySheetCursor`).

Party Sheet's horse picker is not covered — its API does not report that widget, so it
keeps its own frame-locked pointer. That is why `SuppressPartySheetCursor = auto` only
suppresses while this plugin is *active*, rather than for the whole session as Prisma's
does: suppressing unconditionally would leave the horse picker with no pointer at all.

**[Grid Inventory](https://www.nexusmods.com/skyrimspecialedition/mods/188733)** — replaces
the inventory with a Dear ImGui grid, hides the game's Scaleform cursor itself and draws its
own arrow at the end of its frame (`SuppressGridInventoryCursor`).

Nothing extra is needed to make the grid smooth. Its menu carries `kUsesCursor` and opens the
cursor menu itself, so this plugin is already active while it is up and already feeding it
absolute positions — Grid Inventory reads the same `MenuCursor` fields this plugin writes.
Only the second pointer needed solving.

That suppression works differently from the other two, and the difference is worth knowing if
you are updating the signature. Prisma and Party Sheet each keep their cursor draw in a
function of its own, so a `RET` over its first byte is a complete and reversible suppression.
Grid Inventory's `DrawPointer` is inlined into the function that draws its entire interface —
there is no function to stub without taking the whole menu with it. What is still reachable is
the guard that function opens with: ImGui parks an unknown mouse position off screen, and the
pointer is not drawn there. Raising that off-screen threshold to `FLT_MAX` makes the
comparison true for every real cursor position, so the pointer is skipped by Grid Inventory's
own early-out, on the compiler's own branch, with nothing else in the frame touched. It is
also the safer write: four aligned bytes of read-only data, which the render thread reading it
every frame cannot observe half-applied, where the equivalent code patch would be six bytes
over a live instruction.

Because that is a data write rather than a code write, it gets one extra check the other two
do not need — the constant must actually hold the value the guard is documented to compare
against (`-1000.0`) before anything is written. A signature that has drifted onto an unrelated
constant refuses instead of corrupting it.

None of the three is a dependency. The detection half for Party Sheet is API-based and
survives its updates; every suppression half is a signature (verified against **Party Sheet
3.1**, link stamp `0x6A68DAC1`, and **Grid Inventory 1.4.3**) and will need revisiting when
those mods are rebuilt. A stale signature is designed to match nothing: the plugin logs a
warning, leaves the other mod alone, and you get two pointers rather than a crash. The log
lines to check are `resolved its cursor-draw function at +0x...`, `resolved its pointer guard
at +0x...` and `cursor suppressed`.

Grid Inventory has one more layer, because it is updated often and its signature sits inside
an inlined function. If it fails to resolve, this plugin stands its *own* hardware cursor down
while the grid is open rather than leaving two on screen. You get one pointer, drawn by Grid
Inventory at the game's frame rate — still correctly positioned, just not smooth, and only
inside the grid. The log says `the hardware cursor is standing down` when that happens.

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

**Cursor stays on screen after you close a menu, and only alt-tabbing out and back clears
it.** This was a bug in 1.0.1, fixed in 1.0.2. Windows keeps one cursor display counter for
the whole process and the game is not the only thing writing to it — 1.0.1 hooked only the
game executable's import table and hid the pointer exactly once per menu close, so a single
`ShowCursor(true)` from anywhere else stranded it for the rest of the session. If it recurs,
turn on `Verbose` and check that the log's startup line reports the modules it patched;
`EnforceHiddenWhenInactive = false` will confirm whether that half of the fix is what is
holding the cursor down.

**No cursor until you alt-tab out and back.** This was a bug up to 1.0.0 and is fixed. If
it recurs, check the log for `syncTimer=false` on the `Window procedure hooked` line - the
plugin could not start its message-queue timer and is back to needing mouse input before it
notices a menu. `Activated (... showCursorCount=N)` should report `N >= 0`; a negative N
means something is still driving the OS display counter down behind the plugin.

**Crash on reaching the main menu, with `CursorUnbound.dll` in the crash log.** This was a
bug in 1.0.2 and 1.0.3, fixed in 1.0.4. Those versions walked the import table of every
loaded module to find `USER32!ShowCursor`, and trusted the structures they found there; a
module whose import directory is not shaped the way the PE documentation draws it — packed,
proxied, or rewritten by another hooking library — sent the walk off the end of the image.
The log stops after `Patched USER32!ShowCursor in the game import table.` and never reaches
`Patched USER32!ShowCursor in N module(s)`. On 1.0.2 or 1.0.3, `HookAllModules = false`
avoids it. On 1.0.4 the walk is bounds-checked and skips any module it cannot read;
`LogLevel = debug` then names each module as it is swept, which identifies the offender.

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
git tag v1.0.2 && git push origin v1.0.2
```

The tag is checked against `CMakeLists.txt` and the build fails on a mismatch, so a tag
cannot ship an archive labelled with a different version. The release is created as a
draft - review it and publish by hand.

Nexus is not automated. Nexus Mods has no public upload API, so the archive still has to
be uploaded there manually.

## How it works

| Piece | Mechanism |
| --- | --- |
| Position | Vtable detour on `CursorMenu::ProcessMouseMove` (`VTABLE_CursorMenu[1]`, index 4; 6 on 1.7.99 and later). Writes `MenuCursor::cursorPosX/Y` from `GetCursorPos` **before** calling the original, with the event delta zeroed for the duration of that call and restored afterwards. |
| Drawing | `SetVisible(false)` plus `_root._visible = false` and `_root._alpha = 0` on the Cursor Menu movie; the Win32 hardware cursor draws instead. |
| Visibility | IAT patch on `USER32!ShowCursor` to swallow the game's hide calls, plus a 15 ms `WM_TIMER` in a subclassed window procedure that re-asserts the display counter and the cursor image. The timer is what makes this work without mouse input - `WM_SETCURSOR` only arrives once the pointer moves, so on its own it cannot bring the cursor up on a menu that just opened. |
| Art | WIC decode to 32bpp PBGRA, `CreateIconIndirect` with an all-zero AND mask so the alpha channel drives blending. |
| Gamepad | Vtable detour on `ProcessThumbstick` (same vtable, index 3; 5 on 1.7.99 and later). A stick past a deadzone hands the cursor back to the game; any real mouse delta takes it back. |

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

## License

Copyright (c) 2026 Datsferg

Cursor Unbound is free software: you can redistribute it and/or modify it under the terms
of the GNU General Public License as published by the Free Software Foundation, either
version 3 of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
See the GNU General Public License for more details. You should have received a copy of
the GNU General Public License along with this program. If not, see
<https://www.gnu.org/licenses/>.

**This project was MIT-licensed through v1.0.4.** It moved to GPL-3.0-or-later in v1.0.5,
when the CommonLibSSE-NG dependency switched to the maintained
[alandtse fork](https://github.com/alandtse/CommonLibSSE-NG) to gain runtime 1.7.99
support. That fork is GPL-3.0-or-later and is statically linked, so the distributed DLL is
a combined work and has to be distributed under GPL terms. Releases up to and including
v1.0.4 remain available under MIT.

See [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md) for bundled and linked components.
