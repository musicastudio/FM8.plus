; FM8.plus installer (Inno Setup 6). Compile with ISCC after building the shims:
;   iscc /DBuildDir="..\build\Release" installer\FM8.plus.iss
;
; Renames each stock FM8 module to FM8.plus.core and installs the matching proxy in its place;
; sideloads version.dll next to FM8.exe. Uninstall reverses it. Only the 2022-12-23 build is
; patched (checked in code). The PowerShell scripts do the same thing if you prefer them.

#ifndef BuildDir
  #define BuildDir "..\build\Release"
#endif

[Setup]
AppName=FM8.plus
AppVersion=1.0
DefaultDirName={autopf}\FM8.plus
DisableDirPage=yes
PrivilegesRequired=admin
OutputBaseFilename=FM8.plus-Setup
Uninstallable=yes
ArchitecturesInstallIn64BitMode=x64compatible

[Files]
; Staged into the app dir; the [Code] section moves originals aside and copies these into place.
Source: "{#BuildDir}\FM8.dll";     DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildDir}\FM8.vst3";    DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildDir}\version.dll"; DestDir: "{app}"; Flags: ignoreversion

[Code]
const
  EXPECTED_TS = $63A57E00;
  VST2 = 'C:\Program Files\Native Instruments\VSTPlugins 64 bit\FM8.dll';
  VST3 = 'C:\Program Files\Common Files\VST3\FM8.vst3';
  EXEP = 'C:\Program Files\Native Instruments\FM8\FM8.exe';

// Read the PE header timestamp. Inno's TStream.ReadBuffer won't take a raw
// scalar/array the way Delphi does, so slurp the file and index bytes (1-based
// AnsiString). ponytail: LoadStringFromFile reads the whole DLL for a header
// field; fine for a one-time admin install, only the first ~0x120 bytes matter.
function PeTimeStamp(const Path: string): Cardinal;
var s: AnsiString; peOff: Cardinal;
begin
  Result := 0;
  if not LoadStringFromFile(Path, s) then exit;
  if Length(s) < 64 then exit;
  peOff := Cardinal(Ord(s[61])) or (Cardinal(Ord(s[62])) shl 8) or (Cardinal(Ord(s[63])) shl 16) or (Cardinal(Ord(s[64])) shl 24);
  if Cardinal(Length(s)) < peOff + 12 then exit;
  Result := Cardinal(Ord(s[peOff+9])) or (Cardinal(Ord(s[peOff+10])) shl 8) or (Cardinal(Ord(s[peOff+11])) shl 16) or (Cardinal(Ord(s[peOff+12])) shl 24);
end;

function CoreOf(const Orig: string): string;
begin
  Result := ExtractFilePath(Orig) + 'FM8.plus.core';
end;

procedure InstallProxy(const Orig, Staged: string);
var core: string;
begin
  if not FileExists(Orig) then exit;
  core := CoreOf(Orig);
  if not FileExists(core) then
  begin
    if PeTimeStamp(Orig) <> EXPECTED_TS then exit;   // unexpected build, leave alone
    RenameFile(Orig, core);
  end;
  CopyFile(Staged, Orig, False);
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then
  begin
    InstallProxy(VST2, ExpandConstant('{app}\FM8.dll'));
    InstallProxy(VST3, ExpandConstant('{app}\FM8.vst3'));
    if FileExists(EXEP) and (PeTimeStamp(EXEP) = EXPECTED_TS) then
      CopyFile(ExpandConstant('{app}\version.dll'), ExtractFilePath(EXEP) + 'version.dll', False);
  end;
end;

procedure RestoreProxy(const Orig: string);
var core: string;
begin
  core := CoreOf(Orig);
  if FileExists(core) then
  begin
    DeleteFile(Orig);
    RenameFile(core, Orig);
  end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usUninstall then
  begin
    RestoreProxy(VST2);
    RestoreProxy(VST3);
    DeleteFile(ExtractFilePath(EXEP) + 'version.dll');
  end;
end;
