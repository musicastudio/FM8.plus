# FM8.plus

![FM8.plus](docs/screenshot.png)

[![License: GPLv3](https://img.shields.io/github/license/musicastudio/FM8.plus)](https://github.com/musicastudio/FM8.plus/blob/main/LICENSE)
[![Version](https://img.shields.io/github/v/release/musicastudio/FM8.plus?color=7a39fb)](https://github.com/musicastudio/FM8.plus/releases/latest)

An enhancement layer for Native Instruments FM8 (last release 2022-12-23, Windows x64). It adds new features to the standalone `FM8.exe`, the 64-bit VST2 `FM8.dll`, and `FM8.vst3` without modifying a byte of the stock binaries on disk.

## Features

1. **Morph Rotate Control.** Map any MIDI CC (Off, or CC 0..127 with names) to a rotation of the Morph square handle, so one controller sweeps a circle through all four timbres. The on-screen handle follows, and the chosen CC is blocked from its normal FM8 function so it drives only the morph.
2. **Arpeggiator MIDI out.** Three modes for the built-in arpeggiator: **Internal** (stock, nothing leaves), **Clone to MIDI** (FM8 plays the arp and the arp notes are sent to the plugin MIDI output), and **MIDI only** (the arp notes are sent out and FM8's own voices stay silent, so one FM8 can drive another instrument).
3. **Tempo Override.** Unshackle the arp from the DAW tempo: Off, 0.25x, 0.5x, 2x, 4x of host tempo, or Custom (frees FM8's own arp Tempo control). VST2 scales the host time FM8 reads; VST3 scales the process context.
4. **Increase Gain.** Push the output beyond the normal level: Off, +1 dB up to +10 dB, applied post-fader on the plugin output.

The wordmark reads **FM8+**: FM8's own logo shifted left with a matching "+" beside it, drawn on a transparent overlay that shimmers on hover and opens the menu when clicked (one submenu per feature above). Per-instance settings (morph CC, arp mode, tempo, gain) travel with the DAW project via the plugin state; global defaults live in `%APPDATA%\FM8.plus\FM8.plus.ini`.

## Install

Download the latest installer from the [Releases page](https://github.com/musicastudio/FM8.plus/releases/latest) and run it. It needs administrator rights, since FM8 lives under Program Files, and it patches only the 2022-12-23 FM8 build: each stock module is renamed to `FM8.plus.core`, the proxy is dropped in its place, and `version.dll` is sideloaded next to `FM8.exe`. Rescan plugins in your DAW afterwards. Uninstalling from Add/Remove Programs restores stock FM8.

Prefer scripts, or building it yourself? From an elevated PowerShell run `powershell -ExecutionPolicy Bypass -File installer\install.ps1`, and `installer\uninstall.ps1` reverses everything.

## How it was made

FM8 ships only as compiled binaries, so the first job was understanding code nobody has the source to. The three modules (the standalone `FM8.exe`, the VST2 `FM8.dll`, and `FM8.vst3`) were disassembled with [Ghidra](https://ghidra-sre.org/), and the decompiled C was read function by function to locate the internal machinery each feature had to reach, namely the arpeggiator dispatch, the MIDI event handler, the internal Morph X/Y setter, and the form resource that holds the FM8 logo.

That reverse engineering was driven by Claude Fable 5.1. It worked through Ghidra's decompiler output, proposed and adversarially checked where each hook belonged, and confirmed the target functions are byte-identical across all three binaries so one set of detours works in every host. From there it wrote the hook code, the per-host proxies, and the headless test hosts that verify each feature against the real FM8, and the whole thing was built and checked with the model in the loop end to end. The reverse-engineering notes and the exact hook addresses are in [docs/hooks.md](docs/hooks.md) and [docs/design.md](docs/design.md).

To be clear about what that means, this repository contains no Native Instruments source and no decompiled FM8 content. The analysis only informed where FM8.plus attaches its own code at runtime; the stock binaries are never touched on disk.

## How it works

One shared feature core is attached to each host by the least invasive loader that host allows. The VST2 and VST3 proxies keep the original filename; the real module is renamed in place to `FM8.plus.core` and loaded by the proxy, so existing projects keep loading (same uniqueID and class IDs) and gain the features. The standalone uses a `version.dll` sideload, since FM8.exe imports only four version APIs and version.dll is not a KnownDLL, so the app-directory copy wins and FM8.exe is never touched.

The core detours two internal FM8 functions that are byte-identical across all three binaries (the arpeggiator dispatch and the MIDI event handler), captures the arp's generated notes, and sends them out through each host's native path: VST2 `audioMasterProcessEvents`, a VST3 `data.outputEvents` bus that the proxy registers (stock FM8 exposes none), and standalone WinMM `midiOutShortMsg`. The selected morph CC is captured the same way and drives FM8's own internal Morph X/Y setter, tempo is rescaled in the host time FM8 reads, and extra gain is applied to the rendered output buffers. The "FM8" wordmark is a PICTURE control in FM8's form resources, so room for the "+" is made by patching that control's rectangle 11px left in memory before the GUI is built. Hook addresses are in [docs/hooks.md](docs/hooks.md) and [src/core/rvas.h](src/core/rvas.h); the design rationale is in [docs/design.md](docs/design.md).

Every hook is guarded by the FM8 build's PE timestamp; on any mismatch the layer degrades to plain forwarding rather than risk a crash, so a Native Access repair or a different FM8 version is safe.

## Build

Needs Visual Studio 2022 (x64), CMake, and the two vendored submodules.

```bash
git submodule update --init --recursive
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

Outputs in `build\Release`: `FM8.dll` (VST2 proxy), `FM8.vst3` (VST3 proxy), and `version.dll` (standalone sideload). `installer\FM8.plus.iss` is an Inno Setup script that packages them into the setup installer, and `installer\build_installer.ps1` compiles it.

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

All four features are verified end to end against the real FM8.dll with the headless host in `tools/vst2host.py` (arp MIDI out in every mode, morph on an arbitrary CC with the CC blocked from FM8, tempo scaling, and gain), and the GUI is verified in the running standalone. The VST3 event-output bus registration is verified headless, and the standalone reuses the same proven core. A full audio-path pass inside a DAW is the remaining check; see [docs/status.md](docs/status.md).

## Licence

FM8.plus is released under the GNU General Public License v3.0; see [LICENSE](LICENSE).

It is an independent interoperability add-on for software you already own. It ships no Native Instruments code, loads your installed FM8 at runtime, and reverses none of its content into the repository; build and use your own copy, and do not redistribute FM8 itself. The "+" glyph is drawn directly as a slanted cross in our own code, so FM8.plus bundles no third-party font or artwork of any kind.
