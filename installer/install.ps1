# FM8.plus installer. Renames each stock FM8 module to FM8.plus.core (kept as the real engine) and
# drops the matching FM8.plus proxy in its place; sideloads version.dll next to FM8.exe. Reversible
# with uninstall.ps1. Run from an elevated PowerShell (the targets live under Program Files).
#
#   powershell -ExecutionPolicy Bypass -File install.ps1 [-BuildDir <path to build\Release>]
#
# Only the 2022-12-23 build (PE TimeDateStamp 0x63A57E00) is patched; anything else is skipped.
[CmdletBinding()]
param(
  [string]$BuildDir = (Join-Path $PSScriptRoot "..\build\Release"),
  [string]$Vst2 = "C:\Program Files\Native Instruments\VSTPlugins 64 bit\FM8.dll",
  [string]$Vst3 = "C:\Program Files\Common Files\VST3\FM8.vst3",
  [string]$Exe  = "C:\Program Files\Native Instruments\FM8\FM8.exe"
)
$ErrorActionPreference = "Stop"
$EXPECTED_TS = 0x63A57E00

function Get-PeTimeStamp([string]$path) {
  $fs = [System.IO.File]::OpenRead($path)
  try {
    $br = New-Object System.IO.BinaryReader($fs)
    $fs.Position = 0x3C; $peOff = $br.ReadInt32()
    $fs.Position = $peOff + 8   # PE sig (4) + Machine (2) + NumberOfSections (2) = TimeDateStamp
    return $br.ReadUInt32()
  } finally { $fs.Close() }
}

function Install-Proxy([string]$orig, [string]$shim, [string]$label) {
  if (-not (Test-Path $orig)) { Write-Host "[$label] not found at $orig, skipping." -ForegroundColor Yellow; return }
  if (-not (Test-Path $shim)) { throw "[$label] built shim missing: $shim (build the project first)" }
  $core = Join-Path (Split-Path $orig) "FM8.plus.core"
  if (Test-Path $core) { Write-Host "[$label] already installed (FM8.plus.core present), updating proxy only." -ForegroundColor Cyan; Copy-Item $shim $orig -Force; return }
  $ts = Get-PeTimeStamp $orig
  if ($ts -ne $EXPECTED_TS) { Write-Host ("[$label] unexpected build (TimeDateStamp 0x{0:X8}), skipping to stay safe." -f $ts) -ForegroundColor Yellow; return }
  Rename-Item $orig $core
  Copy-Item $shim $orig -Force
  Write-Host "[$label] installed." -ForegroundColor Green
}

function Install-Sideload([string]$exe, [string]$shim, [string]$label) {
  if (-not (Test-Path $exe)) { Write-Host "[$label] FM8.exe not found at $exe, skipping." -ForegroundColor Yellow; return }
  if (-not (Test-Path $shim)) { throw "[$label] built version.dll missing: $shim" }
  $ts = Get-PeTimeStamp $exe
  if ($ts -ne $EXPECTED_TS) { Write-Host ("[$label] unexpected FM8.exe build (0x{0:X8}), skipping." -f $ts) -ForegroundColor Yellow; return }
  Copy-Item $shim (Join-Path (Split-Path $exe) "version.dll") -Force
  Write-Host "[$label] installed (version.dll sideload)." -ForegroundColor Green
}

Write-Host "FM8.plus installer" -ForegroundColor White
Write-Host "Build dir: $BuildDir"
Install-Proxy   $Vst2 (Join-Path $BuildDir "FM8.dll")    "VST2"
Install-Proxy   $Vst3 (Join-Path $BuildDir "FM8.vst3")   "VST3"
Install-Sideload $Exe (Join-Path $BuildDir "version.dll") "Standalone"
Write-Host "`nDone. Rescan plugins in your DAW. Toggles: the 'FM8+' button on the FM8 editor." -ForegroundColor White
