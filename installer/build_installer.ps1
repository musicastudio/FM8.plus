# Compiles installer\FM8.plus.iss into installer\Output\FM8.plus-Windows-Installer.exe.
# Run after building the shims (build\Release must hold FM8.dll, FM8.vst3, version.dll).
#   powershell -ExecutionPolicy Bypass -File installer\build_installer.ps1
$ErrorActionPreference = 'Stop'

$root  = Split-Path $PSScriptRoot -Parent
$build = Join-Path $root 'build\Release'
$build32 = Join-Path $root 'build32\Release'
$iss   = Join-Path $PSScriptRoot 'FM8.plus.iss'

foreach ($f in 'FM8.plus.dll','FM8.plus.vst3','FM8.plus.exe') {
  if (-not (Test-Path (Join-Path $build $f))) {
    throw "Missing $f in $build - build the Release config first (cmake --build build --config Release)."
  }
}
# The 32-bit wrapper for FM8 1.4.1 x86: cmake -B build32 -A Win32; cmake --build build32 --config Release
if (-not (Test-Path (Join-Path $build32 'FM8.plus.dll'))) {
  throw "Missing FM8.plus.dll in $build32 - build the 32-bit config first (cmake -B build32 -A Win32; cmake --build build32 --config Release)."
}
foreach ($f in 'icon_overlay.ico') {
  if (-not (Test-Path (Join-Path $PSScriptRoot $f))) { throw "Missing installer\$f." }
}

# ISCC lands in Program Files or, via winget, under LocalAppData.
$iscc = @(
  "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
  "$env:ProgramFiles\Inno Setup 6\ISCC.exe",
  "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe"
) | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $iscc) { throw "ISCC.exe not found. Install Inno Setup 6 (winget install -e --id JRSoftware.InnoSetup)." }

# CI sets FM8PLUS_VERSION from the release tag; otherwise the .iss default stands.
$defs = @("/DBuildDir=$build", "/DBuildDir32=$build32")
if ($env:FM8PLUS_VERSION) { $defs += "/DAppVer=$env:FM8PLUS_VERSION" }
& $iscc @defs $iss
if ($LASTEXITCODE -ne 0) { throw "ISCC failed with exit code $LASTEXITCODE." }

Write-Host "Built: $(Join-Path $PSScriptRoot 'Output\FM8.plus-Windows-Installer.exe')"
