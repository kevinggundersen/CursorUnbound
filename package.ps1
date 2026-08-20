# Builds a release archive ready to upload to Nexus.
#
#   powershell -ExecutionPolicy Bypass -File package.ps1
#
# Produces release\CursorUnbound-<version>.zip containing only what a user installs.
# Vortex and MO2 both accept a zip with the SKSE\Plugins layout at its root.

param(
    [string]$Version = "1.0.0",
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"

$root    = Split-Path -Parent $MyInvocation.MyCommand.Definition
$dist    = Join-Path $root "dist"
$release = Join-Path $root "release"
$stage   = Join-Path $release "stage"
$cmake   = (Get-Command cmake -ErrorAction SilentlyContinue).Source
if (-not $cmake) {
    # Visual Studio ships CMake but does not put it on PATH.
    $cmake = Get-ChildItem "${env:ProgramFiles}\Microsoft Visual Studio\2022" -Recurse -Filter cmake.exe `
        -ErrorAction SilentlyContinue | Select-Object -First 1 -ExpandProperty FullName
}

if (-not $SkipBuild) {
    Write-Host "==> Building Release" -ForegroundColor Cyan
    & $cmake --build (Join-Path $root "build") --config Release --parallel
    if ($LASTEXITCODE -ne 0) { throw "build failed" }
}

if (-not (Test-Path (Join-Path $dist "SKSE\Plugins\CursorUnbound.dll"))) {
    throw "dist\SKSE\Plugins\CursorUnbound.dll missing - build first"
}

Remove-Item $stage -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $stage | Out-Null

# --- Contents ----------------------------------------------------------------
# Only files a user installs. No tools (they are for patch authors and live in the
# source repo), no .new files, no build output.
Copy-Item (Join-Path $dist "SKSE") $stage -Recurse -Force
Get-ChildItem $stage -Recurse -Include "*.new","*.bak" | Remove-Item -Force -ErrorAction SilentlyContinue

Copy-Item (Join-Path $root "LICENSE") $stage -Force
Copy-Item (Join-Path $root "THIRD-PARTY-NOTICES.md") $stage -Force

$readme = @"
Cursor Unbound $Version
=======================

Decouples the Skyrim menu cursor from the game frame rate. The menu cursor is normally a
Scaleform sprite drawn inside the render frame, so it can never update more often than the
game renders. This hides that sprite, hands drawing to the Windows hardware cursor
(composited at monitor refresh rate, independent of fps) and feeds the OS position back
into the game as an absolute coordinate instead of an fps-scaled delta.

REQUIREMENTS
  - Skyrim SE or AE (built against 1.6.1170; address-library based)
  - SKSE64
  - Address Library for SKSE Plugins
  - Skyrim VR is NOT supported

INSTALL
  Install with a mod manager as normal. Use BORDERLESS WINDOWED rather than exclusive
  fullscreen - the hardware cursor may not composite in exclusive fullscreen, which shows
  up as "no cursor at all". SSE Display Tweaks provides borderless windowed.

CONFIGURING
  SKSE\Plugins\CursorUnbound.ini is commented throughout.
  Log: Documents\My Games\Skyrim Special Edition\SKSE\CursorUnbound.log

CURSOR ART
  Bundled: cursor.png (32px), cursor_48.png, cursor_gold.png, cursor_gold_48.png.
  Point CursorFile at one, or replace cursor.png with your own 32-bit RGBA PNG cropped to
  the pointer. .cur and .ani also work; .ani animates at OS rate.

  Cursor replacer mods ship their art inside cursormenu.swf, which this cannot read, so
  their artwork will not apply automatically. Patches that ship a matching PNG are welcome.

KNOWN TRADE-OFFS
  - Screenshots will not show the cursor. The hardware cursor is not part of the rendered
    frame, so Steam/ENB/in-game captures show menus with no pointer.
  - One static image - no context-sensitive cursor states.
  - Menu hover highlights still update at game fps; only the pointer glide is decoupled.
  - The cursor is confined to the game window while menus are open (ClipToWindow).
  - Gamepad: when a stick drives the cursor the game's own cursor is handed back
    automatically, since the OS pointer cannot follow a thumbstick.

TROUBLESHOOTING
  No cursor at all      -> switch to borderless windowed
  Two cursors           -> HideMethod: render (default) / rootalpha / viewport / all
  Cursor offset from
  where clicks land     -> CoordinateSpace = client, or calibrate with SpanX/SpanY
                           (see the [Debug] section in the ini)

  Reporting a bug: set Verbose = true in the ini and attach CursorUnbound.log.

SOURCE / LICENSE
  MIT. See LICENSE and THIRD-PARTY-NOTICES.md.
  Bundled cursor art derived from ESO Style Cursor under its open permissions.
"@
Set-Content -Path (Join-Path $stage "README.txt") -Value $readme -Encoding UTF8

# --- Archive -----------------------------------------------------------------
$zip = Join-Path $release "CursorUnbound-$Version.zip"
Remove-Item $zip -Force -ErrorAction SilentlyContinue
Compress-Archive -Path (Join-Path $stage "*") -DestinationPath $zip -CompressionLevel Optimal

Remove-Item $stage -Recurse -Force

Write-Host "==> $zip" -ForegroundColor Green
Get-ChildItem $zip | Select-Object Name, @{n='KB';e={[math]::Round($_.Length/1KB,1)}} | Format-Table -AutoSize
