# Configures and builds Cursor Unbound, then optionally deploys it to a mod folder.
#
#   powershell -ExecutionPolicy Bypass -File build.ps1
#   powershell -ExecutionPolicy Bypass -File build.ps1 -ModDir "E:\MO2 Mods\Cursor Unbound"
#
# vcpkg is located via -VcpkgRoot, then $env:VCPKG_ROOT, then a few common paths.
# CMake is located via PATH, then the copy bundled with Visual Studio 2022.
#
# Without -ModDir the build stops after staging into dist\, which is all you need if you
# are not testing in a live install. Pass -Clean to wipe the CMake cache, which is
# required after changing options in CMakeLists.txt.

param(
    [switch]$Clean,
    [string]$Config = "Release",
    [string]$ModDir,
    [string]$VcpkgRoot
)

$ErrorActionPreference = "Stop"

$root  = Split-Path -Parent $MyInvocation.MyCommand.Definition
$build = Join-Path $root "build"
$dist  = Join-Path $root "dist"

# --- Locate toolchain --------------------------------------------------------
if (-not $VcpkgRoot) { $VcpkgRoot = $env:VCPKG_ROOT }
if (-not $VcpkgRoot) {
    $VcpkgRoot = @("C:\vcpkg", "C:\dev\vcpkg", "$env:USERPROFILE\vcpkg") |
        Where-Object { Test-Path (Join-Path $_ "scripts\buildsystems\vcpkg.cmake") } |
        Select-Object -First 1
}
if (-not $VcpkgRoot -or -not (Test-Path (Join-Path $VcpkgRoot "scripts\buildsystems\vcpkg.cmake"))) {
    throw "vcpkg not found. Set VCPKG_ROOT or pass -VcpkgRoot <path>."
}

$cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
if (-not $cmake) {
    # Visual Studio ships CMake but does not put it on PATH.
    $cmake = Get-ChildItem "${env:ProgramFiles}\Microsoft Visual Studio\2022" -Recurse -Filter cmake.exe `
        -ErrorAction SilentlyContinue | Select-Object -First 1 -ExpandProperty FullName
}
if (-not $cmake) { throw "cmake not found on PATH or in Visual Studio 2022." }

Write-Host "    vcpkg: $VcpkgRoot" -ForegroundColor DarkGray
Write-Host "    cmake: $cmake" -ForegroundColor DarkGray

if ($Clean -and (Test-Path $build)) {
    Write-Host "==> Removing $build" -ForegroundColor Cyan
    Remove-Item $build -Recurse -Force
}

# --- Build -------------------------------------------------------------------
Write-Host "==> Configuring" -ForegroundColor Cyan
& $cmake -G "Visual Studio 17 2022" -A x64 -B $build -S $root `
    "-DCMAKE_TOOLCHAIN_FILE=$VcpkgRoot/scripts/buildsystems/vcpkg.cmake" `
    "-DVCPKG_TARGET_TRIPLET=x64-windows-static-md"
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed" }

Write-Host "==> Building ($Config)" -ForegroundColor Cyan
& $cmake --build $build --config $Config --parallel
if ($LASTEXITCODE -ne 0) { throw "Build failed" }

if (-not $ModDir) {
    Write-Host "==> Staged in $dist (no -ModDir given, skipping deploy)" -ForegroundColor Green
    return
}

# --- Deploy ------------------------------------------------------------------
# The DLL is ours and always gets replaced. The INI and cursor art belong to the user once
# they exist - clobbering them silently resets tuned settings and custom art, which is the
# wrong behaviour on a rebuild. Updated defaults land alongside as .new so changes are
# still discoverable.
Write-Host "==> Deploying to $ModDir" -ForegroundColor Cyan
New-Item -ItemType Directory -Force -Path "$ModDir\SKSE\Plugins\CursorUnbound" | Out-Null

Copy-Item "$dist\SKSE\Plugins\CursorUnbound.dll" "$ModDir\SKSE\Plugins\CursorUnbound.dll" -Force

function Deploy-Preserving([string]$Source, [string]$Target) {
    if (-not (Test-Path $Target)) {
        Copy-Item $Source $Target -Force
        Write-Host "    added $(Split-Path $Target -Leaf)" -ForegroundColor DarkGray
        return
    }
    if ((Get-FileHash $Source).Hash -eq (Get-FileHash $Target).Hash) { return }
    Copy-Item $Source "$Target.new" -Force
    Write-Host "    kept existing $(Split-Path $Target -Leaf); new default written as .new" -ForegroundColor Yellow
}

Deploy-Preserving "$dist\SKSE\Plugins\CursorUnbound.ini" "$ModDir\SKSE\Plugins\CursorUnbound.ini"

# Every bundled cursor variant, not just the default one - otherwise CursorFile can point
# at a file that was never deployed.
Get-ChildItem "$dist\SKSE\Plugins\CursorUnbound" -File | ForEach-Object {
    Deploy-Preserving $_.FullName (Join-Path "$ModDir\SKSE\Plugins\CursorUnbound" $_.Name)
}

Write-Host "==> Done" -ForegroundColor Green
