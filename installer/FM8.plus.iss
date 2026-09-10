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
  DEF_VST2 = 'C:\Program Files\Native Instruments\VSTPlugins 64 bit\FM8.dll';
  DEF_VST3 = 'C:\Program Files\Common Files\VST3\FM8.vst3';
  DEF_EXE  = 'C:\Program Files\Native Instruments\FM8\FM8.exe';

var
  LocPage: TInputFileWizardPage;

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

procedure InitializeWizard;
begin
  LocPage := CreateInputFilePage(wpWelcome,
    'FM8 locations',
    'Confirm where FM8 is installed on this machine.',
    'Point each line at your installed FM8 file. Leave a line blank to skip that format.' + #13#10 +
    'Only the 2022-12-23 FM8 build is patched; any other build is left untouched.');
  LocPage.Add('VST2 plug-in (FM8.dll):', 'FM8.dll|FM8.dll', '.dll');
  LocPage.Add('VST3 plug-in (FM8.vst3):', 'FM8.vst3|FM8.vst3', '.vst3');
  LocPage.Add('Standalone (FM8.exe):', 'FM8.exe|FM8.exe', '.exe');
  LocPage.Values[0] := DEF_VST2;
  LocPage.Values[1] := DEF_VST3;
  LocPage.Values[2] := DEF_EXE;
end;

function NextButtonClick(CurPageID: Integer): Boolean;
var i, valid: Integer; p, warn: string;
begin
  Result := True;
  if CurPageID <> LocPage.ID then exit;
  valid := 0; warn := '';
  for i := 0 to 2 do
  begin
    p := Trim(LocPage.Values[i]);
    if p = '' then continue;
    if not FileExists(p) then
      warn := warn + '  - not found: ' + p + #13#10
    else if PeTimeStamp(p) <> EXPECTED_TS then
      warn := warn + '  - not the 2022-12-23 build, will be skipped: ' + p + #13#10
    else
      valid := valid + 1;
  end;
  if valid = 0 then
    Result := (MsgBox('None of these paths point to the supported FM8 build, so nothing will be patched.' + #13#10#13#10 + warn + #13#10 + 'Continue anyway?', mbConfirmation, MB_YESNO) = IDYES)
  else if warn <> '' then
    MsgBox('Some entries will be skipped:' + #13#10#13#10 + warn, mbInformation, MB_OK);
end;

procedure CurStepChanged(CurStep: TSetupStep);
var v2, v3, ex: string;
begin
  if CurStep = ssPostInstall then
  begin
    v2 := Trim(LocPage.Values[0]);
    v3 := Trim(LocPage.Values[1]);
    ex := Trim(LocPage.Values[2]);
    if v2 <> '' then InstallProxy(v2, ExpandConstant('{app}\FM8.dll'));
    if v3 <> '' then InstallProxy(v3, ExpandConstant('{app}\FM8.vst3'));
    if (ex <> '') and FileExists(ex) and (PeTimeStamp(ex) = EXPECTED_TS) then
      CopyFile(ExpandConstant('{app}\version.dll'), ExtractFilePath(ex) + 'version.dll', False);
    // Remember the chosen paths so the uninstaller restores the right files.
    RegWriteStringValue(HKA, 'Software\FM8.plus', 'VST2', v2);
    RegWriteStringValue(HKA, 'Software\FM8.plus', 'VST3', v3);
    RegWriteStringValue(HKA, 'Software\FM8.plus', 'EXE',  ex);
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
var v2, v3, ex: string;
begin
  if CurUninstallStep = usUninstall then
  begin
    if not RegQueryStringValue(HKA, 'Software\FM8.plus', 'VST2', v2) then v2 := DEF_VST2;
    if not RegQueryStringValue(HKA, 'Software\FM8.plus', 'VST3', v3) then v3 := DEF_VST3;
    if not RegQueryStringValue(HKA, 'Software\FM8.plus', 'EXE',  ex) then ex := DEF_EXE;
    if v2 <> '' then RestoreProxy(v2);
    if v3 <> '' then RestoreProxy(v3);
    if ex <> '' then DeleteFile(ExtractFilePath(ex) + 'version.dll');
    RegDeleteKeyIncludingSubkeys(HKA, 'Software\FM8.plus');
  end;
end;
