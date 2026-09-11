# FM8.plus installer (script alternative to the Setup.exe). Installs FM8.plus as its OWN files next
# to your existing FM8, and creates the shortcuts. It never renames, copies, or modifies a stock FM8
# file, so a Native Access repair or update cannot break it. Run from an elevated PowerShell.
#
#   powershell -ExecutionPolicy Bypass -File install.ps1 [-BuildDir <build\Release>] [-Vst2 ..] [-Vst3 ..] [-Exe ..]
[CmdletBinding()]
param(
  [string]$BuildDir = (Join-Path $PSScriptRoot "..\build\Release"),
  [string]$Vst2 = "C:\Program Files\Native Instruments\VSTPlugins 64 bit\FM8.dll",
  [string]$Vst3 = "C:\Program Files\Common Files\VST3\FM8.vst3",
  [string]$Exe  = "C:\Program Files\Native Instruments\FM8\FM8.exe"
)
$ErrorActionPreference = "Stop"

$dll  = Join-Path $BuildDir "FM8.plus.dll"
$vst3 = Join-Path $BuildDir "FM8.plus.vst3"
$exe  = Join-Path $BuildDir "FM8.plus.exe"

function Copy-To([string]$src, [string]$destDir, [string]$label) {
  if (-not (Test-Path $src)) { throw "$label source missing: $src (build the project first)" }
  Copy-Item $src (Join-Path $destDir (Split-Path $src -Leaf)) -Force
  Write-Host "[$label] -> $destDir" -ForegroundColor Green
}

Write-Host "FM8.plus installer (installs alongside FM8; stock FM8 untouched)" -ForegroundColor White

if (Test-Path $Vst2) { Copy-To $dll (Split-Path $Vst2) "VST2 wrapper" }
else { Write-Host "[VST2] FM8.dll not found at $Vst2, skipping" -ForegroundColor Yellow }

if (Test-Path $Vst3) { Copy-To $vst3 (Split-Path $Vst3) "VST3 wrapper" }
else { Write-Host "[VST3] FM8.vst3 not found at $Vst3, skipping" -ForegroundColor Yellow }

if (Test-Path $Exe) {
  $exeDir = Split-Path $Exe
  Copy-To $exe $exeDir "Standalone launcher"
  Copy-To $dll $exeDir "Standalone DLL"
  $target = Join-Path $exeDir "FM8.plus.exe"
  $ws = New-Object -ComObject WScript.Shell
  $links = @(
    (Join-Path ([Environment]::GetFolderPath('CommonDesktopDirectory')) "FM8 Plus.lnk"),
    (Join-Path $env:ProgramData "Microsoft\Windows\Start Menu\Programs\Native Instruments\FM8\FM8 Plus.lnk"))
  foreach ($lnk in $links) {
    $dir = Split-Path $lnk
    if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
    $s = $ws.CreateShortcut($lnk)
    $s.TargetPath = $target
    $s.WorkingDirectory = $exeDir
    $s.IconLocation = "$target,0"
    $s.Description = "FM8 with the FM8.plus features"
    $s.Save()
    Write-Host "[Shortcut] $lnk" -ForegroundColor Green
  }
} else { Write-Host "[Standalone] FM8.exe not found at $Exe, skipping" -ForegroundColor Yellow }

Write-Host "`nDone. Rescan plugins in your DAW; FM8+ appears alongside FM8. Launch the standalone from the FM8 Plus shortcut." -ForegroundColor White
