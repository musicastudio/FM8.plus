# FM8.plus uninstaller (script). Removes the FM8.plus files and shortcuts. Stock FM8 is untouched
# (it was never modified), so there is nothing to restore. Run from an elevated PowerShell.
#
#   powershell -ExecutionPolicy Bypass -File uninstall.ps1 [-Vst2 ..] [-Vst3 ..] [-Exe ..]
[CmdletBinding()]
param(
  [string]$Vst2 = "C:\Program Files\Native Instruments\VSTPlugins 64 bit\FM8.dll",
  [string]$Vst3 = "C:\Program Files\Common Files\VST3\FM8.vst3",
  [string]$Exe  = "C:\Program Files\Native Instruments\FM8\FM8.exe"
)
$ErrorActionPreference = "Stop"

function Del([string]$p, [string]$label) {
  if (Test-Path $p) { Remove-Item $p -Force; Write-Host "[$label] removed $p" -ForegroundColor Green }
  else { Write-Host "[$label] not present, skipping" -ForegroundColor Yellow }
}

Write-Host "FM8.plus uninstaller" -ForegroundColor White
Del (Join-Path (Split-Path $Vst2) "FM8.plus.dll")  "VST2 wrapper"
Del (Join-Path (Split-Path $Vst3) "FM8.plus.vst3") "VST3 wrapper"
$exeDir = Split-Path $Exe
Del (Join-Path $exeDir "FM8.plus.exe") "Standalone launcher"
Del (Join-Path $exeDir "FM8.plus.dll") "Standalone DLL"
Del (Join-Path $exeDir "FM8.plus.ico") "Composited icon"
Del (Join-Path ([Environment]::GetFolderPath('CommonDesktopDirectory')) "FM8 Plus.lnk") "Desktop shortcut"
Del (Join-Path $env:ProgramData "Microsoft\Windows\Start Menu\Programs\Native Instruments\FM8\FM8 Plus.lnk") "Start Menu shortcut"

Write-Host "`nDone. FM8 is stock (it was never modified). You may also delete %APPDATA%\FM8.plus." -ForegroundColor White
