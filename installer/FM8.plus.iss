; FM8.plus installer (Inno Setup 6). Compile with ISCC after building the binaries:
;   iscc /DBuildDir="..\build\Release" installer\FM8.plus.iss
; or run installer\build_installer.ps1.
;
; FM8.plus installs as its own set of files next to the UNTOUCHED stock FM8:
;   - FM8.plus.dll   into the VST2 folder (beside stock FM8.dll), 64-bit and 32-bit
;   - FM8.plus.vst3  into the VST3 folder (beside stock FM8.vst3)
;   - FM8.plus.exe   (+ FM8.plus.dll) into FM8's program folder (beside stock FM8.exe)
; plus a desktop and Start Menu shortcut to the launcher. No stock file is renamed, copied, or
; modified, so a Native Access reinstall cannot break FM8.plus and uninstalling just removes our
; files. The wrappers load the real FM8 in place and present themselves as the distinct plug-in
; "FM8.plus"; the launcher starts FM8.exe with the standalone features injected.

#ifndef BuildDir
  #define BuildDir "..\build\Release"
#endif
; The 32-bit VST2 wrapper, for FM8 1.4.1's x86 plug-in (the last 32-bit build NI shipped).
#ifndef BuildDir32
  #define BuildDir32 "..\build32\Release"
#endif

[Setup]
AppName=FM8.plus
AppVersion=1.0.5
AppPublisher=Musica Studio
DefaultDirName={autopf}\FM8.plus
DisableDirPage=yes
DisableProgramGroupPage=yes
PrivilegesRequired=admin
OutputBaseFilename=FM8.plus-Setup
UninstallDisplayIcon={app}\FM8.plus.ico
Uninstallable=yes
ArchitecturesInstallIn64BitMode=x64compatible

[Files]
; The FM8+ icon is FM8's own program icon with our "+" overlay, so we must NOT redistribute it.
; We ship only our overlay and the helper. The icon is composited on this machine at install time
; from the user's own licensed FM8.exe (MakeIcon) and then injected into their installed copy of
; FM8.plus.exe (EmbedIcon), so the distributed binary stays free of Native Instruments artwork.
Source: "icon_overlay.ico";      Flags: dontcopy
Source: "..\tools\make_icon.ps1"; Flags: dontcopy
; VST2 wrapper -> the folder that holds the chosen FM8.dll.
Source: "{#BuildDir}\FM8.plus.dll";  DestDir: "{code:DirVst2}"; Flags: ignoreversion; Check: DoVst2
; VST3 wrapper -> the folder that holds the chosen FM8.vst3.
Source: "{#BuildDir}\FM8.plus.vst3"; DestDir: "{code:DirVst3}"; Flags: ignoreversion; Check: DoVst3
; 32-bit VST2 wrapper -> the folder that holds the chosen 32-bit FM8.dll.
Source: "{#BuildDir32}\FM8.plus.dll"; DestDir: "{code:DirVst2x86}"; Flags: ignoreversion; Check: DoVst2x86
; Launcher + the DLL it injects -> FM8's program folder (beside FM8.exe).
Source: "{#BuildDir}\FM8.plus.exe";  DestDir: "{code:DirExe}"; Flags: ignoreversion; Check: DoExe
Source: "{#BuildDir}\FM8.plus.dll";  DestDir: "{code:DirExe}"; Flags: ignoreversion; Check: DoExe

[Icons]
; Start Menu entry next to FM8's own, and a desktop shortcut. Both launch FM8.plus and use the icon
; composited on this machine (falling back to the launcher's own if compositing did not run).
Name: "{commonprograms}\Native Instruments\FM8\FM8 Plus"; Filename: "{code:PathExe}"; WorkingDir: "{code:DirExe}"; IconFilename: "{code:IconPath}"; Comment: "FM8 with the FM8.plus features"; Check: DoExe
Name: "{autodesktop}\FM8 Plus"; Filename: "{code:PathExe}"; WorkingDir: "{code:DirExe}"; IconFilename: "{code:IconPath}"; Comment: "FM8 with the FM8.plus features"; Check: DoExe

[UninstallDelete]
; The icon is generated at install time, so Inno does not track it for removal.
Type: files; Name: "{app}\FM8.plus.ico"

[Code]
const
  DEF_VST2 = 'C:\Program Files\Native Instruments\VSTPlugins 64 bit\FM8.dll';
  DEF_VST2_X86 = 'C:\Program Files\Native Instruments\VSTPlugins 32 bit\FM8.dll';
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

// The builds FM8.plus enhances: 1.4.6 of 2022-12-23 (one stamp for all three binaries) and
// 1.4.1 of 2015-10-20, which stamps its EXE, its x64 plug-in and its x86 plug-in separately.
function Supported(ts: Cardinal): Boolean;
begin
  Result := (ts = $63A57E00) or (ts = $56266040) or (ts = $56266059) or (ts = $56265F2B);
end;

function Val(i: Integer): string;
begin
  Result := Trim(LocPage.Values[i]);
end;

function DirVst2(Param: string): string; begin Result := ExtractFileDir(Val(0)); end;
function DirVst3(Param: string): string; begin Result := ExtractFileDir(Val(1)); end;
function DirExe (Param: string): string; begin Result := ExtractFileDir(Val(2)); end;
function PathExe(Param: string): string; begin Result := ExtractFileDir(Val(2)) + '\FM8.plus.exe'; end;

function DirVst2x86(Param: string): string; begin Result := ExtractFileDir(Val(3)); end;

function DoVst2: Boolean; begin Result := (Val(0) <> '') and FileExists(Val(0)); end;
function DoVst2x86: Boolean; begin Result := (Val(3) <> '') and FileExists(Val(3)); end;
function DoVst3: Boolean; begin Result := (Val(1) <> '') and FileExists(Val(1)); end;
function DoExe:  Boolean; begin Result := (Val(2) <> '') and FileExists(Val(2)); end;

// Shortcut icon: the composited FM8+ icon if we managed to build it, else the launcher itself.
function IconPath(Param: string): string;
begin
  Result := ExpandConstant('{app}\FM8.plus.ico');
  if not FileExists(Result) then Result := ExtractFileDir(Val(2)) + '\FM8.plus.exe';
end;

// Build the FM8+ icon HERE, on the user's machine, from their own FM8.exe plus our "+" overlay.
// FM8's icon is Native Instruments' artwork, so it is never shipped in this installer or the repo.
// Runs at ssInstall, before [Icons], so the shortcuts can point at the result. Failure is harmless:
// IconPath then falls back to the launcher.
procedure MakeIcon;
var rc: Integer; args: string;
begin
  if not DoExe then exit;
  ExtractTemporaryFile('icon_overlay.ico');
  ExtractTemporaryFile('make_icon.ps1');
  ForceDirectories(ExpandConstant('{app}'));
  args := '-NoProfile -ExecutionPolicy Bypass -File "' + ExpandConstant('{tmp}\make_icon.ps1') + '"'
        + ' -Fm8Exe "'  + Val(2) + '"'
        + ' -Overlay "' + ExpandConstant('{tmp}\icon_overlay.ico') + '"'
        + ' -Out "'     + ExpandConstant('{app}\FM8.plus.ico') + '"';
  Exec('powershell.exe', args, '', SW_HIDE, ewWaitUntilTerminated, rc);
end;

// Inject the composited icon into the INSTALLED launcher so Explorer shows it on the .exe too, not
// just on the shortcuts. Runs at ssPostInstall, once [Files] has copied FM8.plus.exe into place.
// The binary we distribute stays icon-free; the artwork only ever exists on the user's machine.
// Failure is harmless: the shortcuts already point at {app}\FM8.plus.ico.
procedure EmbedIcon;
var rc: Integer; args: string;
begin
  if not DoExe then exit;
  if not FileExists(ExpandConstant('{app}\FM8.plus.ico')) then exit;
  ExtractTemporaryFile('make_icon.ps1');
  args := '-NoProfile -ExecutionPolicy Bypass -File "' + ExpandConstant('{tmp}\make_icon.ps1') + '"'
        + ' -EmbedOnly'
        + ' -Out "'       + ExpandConstant('{app}\FM8.plus.ico') + '"'
        + ' -EmbedInto "' + ExtractFileDir(Val(2)) + '\FM8.plus.exe"';
  Exec('powershell.exe', args, '', SW_HIDE, ewWaitUntilTerminated, rc);
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssInstall then MakeIcon;
  if CurStep = ssPostInstall then EmbedIcon;
end;

procedure InitializeWizard;
begin
  LocPage := CreateInputFilePage(wpWelcome,
    'FM8 locations',
    'Confirm where FM8 is installed on this machine.',
    'Point each line at your installed FM8 file. Leave a line blank to skip that format.' + #13#10 +
    'FM8.plus installs alongside these files and never modifies them. The 1.4.6 (2022-12-23) and' + #13#10 +
    '1.4.1 (2015-10-20) builds are enhanced; any other build is loaded but left as plain FM8.');
  LocPage.Add('VST2 plug-in (FM8.dll):', 'FM8.dll|FM8.dll', '.dll');
  LocPage.Add('VST3 plug-in (FM8.vst3):', 'FM8.vst3|FM8.vst3', '.vst3');
  LocPage.Add('Standalone (FM8.exe):', 'FM8.exe|FM8.exe', '.exe');
  LocPage.Add('VST2 plug-in, 32-bit (FM8.dll):', 'FM8.dll|FM8.dll', '.dll');
  LocPage.Values[0] := DEF_VST2;
  LocPage.Values[1] := DEF_VST3;
  LocPage.Values[2] := DEF_EXE;
  LocPage.Values[3] := DEF_VST2_X86;
end;

function NextButtonClick(CurPageID: Integer): Boolean;
var i, valid: Integer; p, warn: string;
begin
  Result := True;
  if CurPageID <> LocPage.ID then exit;
  valid := 0; warn := '';
  for i := 0 to 3 do
  begin
    p := Trim(LocPage.Values[i]);
    if p = '' then continue;
    if not FileExists(p) then
      warn := warn + '  - not found: ' + p + #13#10
    else if not Supported(PeTimeStamp(p)) then
      warn := warn + '  - not a supported build, will load as plain FM8: ' + p + #13#10
    else
      valid := valid + 1;
  end;
  if valid = 0 then
    Result := (MsgBox('None of these paths point to a supported FM8 build.' + #13#10#13#10 + warn + #13#10 + 'Install anyway?', mbConfirmation, MB_YESNO) = IDYES)
  else if warn <> '' then
    MsgBox('Some entries will be skipped or run as plain FM8:' + #13#10#13#10 + warn, mbInformation, MB_OK);
end;
