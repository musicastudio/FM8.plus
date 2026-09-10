# FM8.plus uninstaller. Reverses install.ps1: restores each FM8.plus.core back to the original name
# and removes the version.dll sideload. Run elevated.
#
#   powershell -ExecutionPolicy Bypass -File uninstall.ps1
[CmdletBinding()]
param(
  [string]$Vst2 = "C:\Program Files\Native Instruments\VSTPlugins 64 bit\FM8.dll",
  [string]$Vst3 = "C:\Program Files\Common Files\VST3\FM8.vst3",
  [string]$Exe  = "C:\Program Files\Native Instruments\FM8\FM8.exe"
)
$ErrorActionPreference = "Stop"

function Restore-Proxy([string]$orig, [string]$label) {
  $core = Join-Path (Split-Path $orig) "FM8.plus.core"
  if (-not (Test-Path $core)) { Write-Host "[$label] not installed (no FM8.plus.core), skipping." -ForegroundColor Yellow; return }
  if (Test-Path $orig) { Remove-Item $orig -Force }   # our proxy
  Rename-Item $core $orig
  Write-Host "[$label] restored." -ForegroundColor Green
}

function Remove-Sideload([string]$exe, [string]$label) {
  $ver = Join-Path (Split-Path $exe) "version.dll"
  if (Test-Path $ver) { Remove-Item $ver -Force; Write-Host "[$label] version.dll removed." -ForegroundColor Green }
  else { Write-Host "[$label] no version.dll, skipping." -ForegroundColor Yellow }
}

Write-Host "FM8.plus uninstaller" -ForegroundColor White
Restore-Proxy $Vst2 "VST2"
Restore-Proxy $Vst3 "VST3"
Remove-Sideload $Exe "Standalone"
Write-Host "`nDone. FM8 is back to stock. You may also delete %APPDATA%\FM8.plus." -ForegroundColor White
