# FM8.plus

An enhancement layer for Native Instruments FM8 (last release 2022-12-23, Windows x64). It adds two
long-requested features to the standalone `FM8.exe`, the 64-bit VST2 `FM8.dll`, and `FM8.vst3`
without modifying a byte of the stock binaries on disk.

## Features

1. **Mod wheel morphs.** A toggle that maps the mod wheel (CC1) to a rotation of the Morph square
   handle, so one controller sweeps a circle through all four timbres. The on-screen handle follows.
2. **Arpeggiator MIDI out.** A toggle with three modes for the built-in arpeggiator:
   - **Internal** stock behaviour, nothing leaves.
   - **Clone to MIDI** FM8 plays the arp AND the arp notes are sent to the plugin MIDI output.
   - **MIDI only** the arp notes are sent out and FM8's own voices stay silent, so one FM8 can drive
     another instrument.

Toggles live on a small **FM8+** button drawn on the FM8 editor. Per-instance settings (mod-wheel
toggle, arp mode) travel with the DAW project; global knobs live in `%APPDATA%\FM8.plus\FM8.plus.ini`.

## How it works

One shared feature core is attached to each host by the least invasive loader that host allows:

- **VST2 / VST3**: a proxy DLL keeps the original's name; the real module is renamed in place to
  `FM8.plus.core` and loaded by the proxy, so existing projects keep loading (same uniqueID and class
  IDs) and gain the features.
- **Standalone**: a `version.dll` sideload (FM8.exe imports only four version APIs and version.dll is
  not a KnownDLL, so the app-directory copy wins); FM8.exe is untouched and its shortcut still works.

The core detours two internal FM8 functions that are byte-identical across all three binaries (the
arp dispatch and the MIDI event handler), captures the arp's generated notes, and sends them out
through each host's native path: VST2 `audioMasterProcessEvents`, VST3 a `data.outputEvents` bus that
the proxy registers (stock FM8 exposes none), and standalone WinMM `midiOutShortMsg`. The mod wheel
drives FM8's own Morph X/Y setter. Hook addresses are in [docs/hooks.md](docs/hooks.md) and
[src/core/rvas.h](src/core/rvas.h); the design rationale is in [docs/design.md](docs/design.md).

Every hook is guarded by the FM8 build's PE timestamp; on any mismatch the layer degrades to plain
forwarding rather than risk a crash, so a Native Access repair or a different FM8 version is safe.

## Build

Needs Visual Studio 2022 (x64), CMake, and the two vendored submodules.

```bash
git submodule update --init --recursive
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

Outputs in `build\Release`: `FM8.dll` (VST2 proxy), `FM8.vst3` (VST3 proxy), `version.dll`
(standalone sideload).

## Install

From an elevated PowerShell (the targets live under Program Files):

```powershell
powershell -ExecutionPolicy Bypass -File installer\install.ps1
```

It renames each stock module to `FM8.plus.core`, drops the proxy in its place, and sideloads
`version.dll` next to `FM8.exe`, patching only the 2022-12-23 build. Rescan plugins in your DAW.
`installer\uninstall.ps1` reverses everything. `installer\FM8.plus.iss` is an Inno Setup script that
does the same as a distributable installer.

## Layout

```
FM8.plus/
  src/core/         shared feature core (hooks, arp routing, morph, settings, UI, rvas.h)
  src/shim_vst2/    VST2 proxy  -> FM8.dll
  src/shim_vst3/    VST3 proxy  -> FM8.vst3
  src/shim_exe/     standalone  -> version.dll
  src/vst2/         minimal VST 2.4 ABI header
  docs/             design, reverse-engineering notes, hook reference, status
  tools/            pyghidra build + query helpers and the headless VST2/VST3 test hosts
  installer/        PowerShell + Inno Setup installers
../FM8_DISASM/      binaries and Ghidra projects (not in git)
```

## Status

See [docs/status.md](docs/status.md). The VST2 path is verified end to end against the real plugin
with the headless host in `tools/vst2host.py`; the VST3 event-out bus registration is verified
headless; the standalone builds and reuses the same proven core. Full audio-path confirmation in a
DAW is the remaining check.

## Legal

FM8.plus is an independent interoperability add-on for software you already own. It ships no Native
Instruments code; it loads your installed FM8 at runtime and reverses none of its content into the
repository. Distribute your own build; do not redistribute FM8.
