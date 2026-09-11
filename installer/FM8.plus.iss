; FM8.plus installer (Inno Setup 6). Compile with ISCC after building the binaries:
;   iscc /DBuildDir="..\build\Release" installer\FM8.plus.iss
; or run installer\build_installer.ps1.
;
; FM8.plus installs as its own set of files next to the UNTOUCHED stock FM8:
;   - FM8.plus.dll   into the VST2 folder (beside stock FM8.dll)
;   - FM8.plus.vst3  into the VST3 folder (beside stock FM8.vst3)
;   - FM8.plus.exe   (+ FM8.plus.dll) into FM8's program folder (beside stock FM8.exe)
; plus a desktop and Start Menu shortcut to the launcher. No stock file is renamed, copied, or
; modified, so a Native Access reinstall cannot break FM8.plus and uninstalling just removes our
; files. The wrappers load the real FM8 in place and present themselves as the distinct plug-in
; "FM8+"; the launcher starts FM8.exe with the standalone features injected.

#ifndef BuildDir
  #define BuildDir "..\build\Release"
#endif

[Setup]
AppName=FM8.plus
AppVersion=1.0
AppPublisher=Musica Studio
DefaultDirName={autopf}\FM8.plus
DisableDirPage=yes
DisableProgramGroupPage=yes
PrivilegesRequired=admin
OutputBaseFilename=FM8.plus-Setup
SetupIconFile=FM8.plus.ico
UninstallDisplayIcon={app}\FM8.plus.ico
Uninstallable=yes
ArchitecturesInstallIn64BitMode=x64compatible

[Files]
; A copy of the icon in the app dir gives the uninstaller a display icon.
Source: "FM8.plus.ico"; DestDir: "{app}"; Flags: ignoreversion
; VST2 wrapper -> the folder that holds the chosen FM8.dll.
Source: "{#BuildDir}\FM8.plus.dll";  DestDir: "{code:DirVst2}"; Flags: ignoreversion; Check: DoVst2
; VST3 wrapper -> the folder that holds the chosen FM8.vst3.
Source: "{#BuildDir}\FM8.plus.vst3"; DestDir: "{code:DirVst3}"; Flags: ignoreversion; Check: DoVst3
; Launcher + the DLL it injects -> FM8's program folder (beside FM8.exe).
Source: "{#BuildDir}\FM8.plus.exe";  DestDir: "{code:DirExe}"; Flags: ignoreversion; Check: DoExe
Source: "{#BuildDir}\FM8.plus.dll";  DestDir: "{code:DirExe}"; Flags: ignoreversion; Check: DoExe

[Icons]
; Start Menu entry next to FM8's own, and a desktop shortcut. Both launch FM8+.
Name: "{commonprograms}\Native Instruments\FM8\FM8 Plus"; Filename: "{code:PathExe}"; WorkingDir: "{code:DirExe}"; IconFilename: "{code:PathExe}"; Comment: "FM8 with the FM8.plus features"; Check: DoExe
Name: "{autodesktop}\FM8 Plus"; Filename: "{code:PathExe}"; WorkingDir: "{code:DirExe}"; IconFilename: "{code:PathExe}"; Comment: "FM8 with the FM8.plus features"; Check: DoExe

[Code]
const
  EXPECTED_TS = $63A57E00;
  DEF_VST2 = 'C:\Program Files\Native Instruments\VSTPlugins 64 bit\FM8.dll';
  DEF_VST3 = 'C:\Program Files\Common Files\VST3\FM8.vst3';
  DEF_EXE  = 'C:\Program Files\Native Instruments\FM8\FM8.exe';

var
  LocPage: TInputFileWizardPage;

// Read the PE header timestamp. Inno's TStream.ReadBuffer won't take a raw scalar/array the way
// Delphi does, so slurp the file and index bytes (1-based AnsiString). Only the first ~0x120 bytes
// matter; this is advisory (we never patch stock, but we warn if it is not the supported build).
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

function Val(i: Integer): string;
begin
  Result := Trim(LocPage.Values[i]);
end;

function DirVst2(Param: string): string; begin Result := ExtractFileDir(Val(0)); end;
function DirVst3(Param: string): string; begin Result := ExtractFileDir(Val(1)); end;
function DirExe (Param: string): string; begin Result := ExtractFileDir(Val(2)); end;
function PathExe(Param: string): string; begin Result := ExtractFileDir(Val(2)) + '\FM8.plus.exe'; end;

function DoVst2: Boolean; begin Result := (Val(0) <> '') and FileExists(Val(0)); end;
function DoVst3: Boolean; begin Result := (Val(1) <> '') and FileExists(Val(1)); end;
function DoExe:  Boolean; begin Result := (Val(2) <> '') and FileExists(Val(2)); end;

procedure InitializeWizard;
begin
  LocPage := CreateInputFilePage(wpWelcome,
    'FM8 locations',
    'Confirm where FM8 is installed on this machine.',
    'Point each line at your installed FM8 file. Leave a line blank to skip that format.' + #13#10 +
    'FM8.plus installs alongside these files and never modifies them; only the 2022-12-23 build is' + #13#10 +
    'enhanced (any other build is loaded but left as plain FM8).');
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
      warn := warn + '  - not the 2022-12-23 build, will load as plain FM8: ' + p + #13#10
    else
      valid := valid + 1;
  end;
  if valid = 0 then
    Result := (MsgBox('None of these paths point to the supported FM8 build.' + #13#10#13#10 + warn + #13#10 + 'Install anyway?', mbConfirmation, MB_YESNO) = IDYES)
  else if warn <> '' then
    MsgBox('Some entries will be skipped or run as plain FM8:' + #13#10#13#10 + warn, mbInformation, MB_OK);
end;
