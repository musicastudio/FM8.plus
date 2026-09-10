# FM8.plus

Enhancement layer for Native Instruments FM8 (last release 2022, Windows). Covers the
standalone `FM8.exe`, the 64-bit VST2 `FM8.dll`, and `FM8.vst3`.

## Features

1. **Mod wheel morph rotation.** A toggle that maps the mod wheel (CC1) to a rotation
   around the Morph square, so one controller sweeps through all four timbres.
2. **Arpeggiator MIDI out.** A toggle with three modes for the built-in arpeggiator:
   `Internal` (stock behaviour), `Clone to MIDI` (play internally and send the arp
   notes to the plugin MIDI output), and `MIDI only` (send only, silence the internal
   voices).

## Layout

```
FM8.plus/                  this repo
  tools/build_fm8_ghidra.py   import + analyze + decompile the three binaries
  docs/                       reverse-engineering notes and findings
../FM8_DISASM/             binaries and Ghidra projects (not in git)
  FM8_EXE/FM8.exe
  FM8_VST3/FM8.vst3
  FM8_VST_64/FM8.dll, FM8 FX.dll
  <name>_GHIDRA_PROJ/       Ghidra project per binary
  <name>_GHIDRA_ANALYSIS/   decomp.db per binary (SQLite, one row per function)
```

## Toolchain

Ghidra 12.1.2 at `E:\ghidra_12.1.2_PUBLIC`, pyghidra 3.1.0, Python 3.13.
