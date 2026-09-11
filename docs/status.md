# Status

What is verified, and what still needs a real DAW. Honest accounting, no overclaiming.

## Verified

**Reverse engineering.** All three binaries decompiled (Ghidra 12.1.2), every hook located,
adversarially verified, and cross-ported. Each hooked function resolves at its recorded address with
a byte-identical size across exe / vst2 / vst3 (see `tools/q.py` and the check in the build history).

**VST2, end to end against the installed FM8** (the FM8+ wrapper loading stock FM8.dll from its own
folder, headless host `tools/vst2host.py plus`):

| Check | Result |
|-------|--------|
| Wrapper loads stock FM8 in place, forwards, reports its own identity | pass (uniqueID 0x466d382b 'Fm8+', 1094 params) |
| Morph Rotate Control on an arbitrary CC | pass (CC 11 traces the full circle; CC1 inert, and the mapped CC is blocked from FM8) |
| Arp Clone to MIDI: audio plays and notes sent to host | pass (17 note-ons, audio 0.073) |
| Arp MIDI only: notes sent, FM8 voices silenced | pass (16 note-ons, audio 0.000) |
| Arp Internal: nothing leaks to MIDI out | pass (0 events) |
| Tempo Override 2x / 0.5x | pass (arp note density 18 -> 37 at 2x, -> 9 at 0.5x) |
| Increase Gain +6 dB / +10 dB | pass (output peak x2.00 and x3.16, matching the dB) |
| Chunk trailer round-trips per-instance state | pass (trailer stripped so FM8 only ever sees its own bytes) |

A flaky crash seen while adding these was a use-after-free in the test host (it freed the VstEvents
buffer before FM8 read it during processReplacing; FM8 stores the pointer), fixed in
`tools/vst2host.py`. It was never in the shim.

**VST3, headless** (`tools/vst3host.py`): the wrapper loads stock FM8.vst3 in place and wraps its
factory to present FM8's two audio classes under distinct FM8+ identities, so it coexists with stock
FM8. Instances created through our factory get the added event-output bus (stock FM8 exposes none);
plain FM8 instances are left alone.

```
factory classes: 2
  [0] Audio Module Class  FM8+     cid=ce54...6669...   (stock FM8 is 4e54...6669...)
  [1] Audio Module Class  FM8 FX+  cid=ce54...8669...
event out: 1 bus(es)
    [0] 'FM8+ Arp Out' channels=16 busType=0 flags=0x1
```

**Standalone**: the `FM8.plus.exe` launcher starts the untouched `FM8.exe` suspended, injects
`FM8.plus.dll` (its `DllMain` detects the FM8.exe host and runs the standalone attach), and resumes.
Confirmed on a real run: FM8.exe starts, our DLL is loaded into it, and the attach thread runs (the
`%APPDATA%\FM8.plus` settings dir is created). The attach reuses the same core proven under VST2 and
the same overlay proven in the earlier `version.dll` build (logo shift + the drawn **FM8+** "+"
and the four-submenu menu). Plain `FM8.exe`, launched normally, is untouched.

**Installer**: `installer\FM8.plus.iss` (Inno Setup) and `install.ps1` / `uninstall.ps1` install
FM8.plus as its own files beside stock FM8 (`FM8.plus.dll` in the VST2 folder, `FM8.plus.vst3` in the
VST3 folder, `FM8.plus.exe` + `FM8.plus.dll` beside `FM8.exe`) plus desktop and Start Menu shortcuts.
No stock file is renamed, copied, or modified, so uninstall just removes the added files. The Inno
installer compiles clean and the PE-timestamp build gate is verified against the installed binaries.

## Needs a DAW to confirm

- **VST3 arp MIDI out audio path.** The bus registers headless; routing the notes through a real
  host (Cubase, Reaper, Bitwig, Studio One) and hearing/receiving them is unverified. Some hosts
  cache bus layouts and need one plugin rescan after install.
- **VST3 morph on the mapped CC** relies on the host delivering the CC's parameter id (found via
  `IMidiMapping`) in `inputParameterChanges`; confirmed as the mapping target, not yet observed live.
- **Tempo Override and Increase Gain** are verified headless under VST2; the VST3 process-context
  tempo scaling and both features under the standalone still want a listening test in a DAW.
- **Standalone** overlay button and menu confirmed on a real run (see above); the arp-out to a WinMM
  port and the actual audio still need a listening test with a MIDI monitor.
- **VST2 host coverage.** Plugin-to-host MIDI is not routed by every host; where a host drops it,
  Clone/MIDI-only produce nothing downstream (the mode is labelled honestly).

## Known limitations / follow-ups

- **Coexistence with plain FM8 in one process.** FM8+ loads the stock FM8 module in place and shares
  it, so the feature hooks are gated to FM8+ instances (by `Core::current` in VST2, by an instance
  registry in VST3) and plain FM8 stays stock. The one shared side effect is the logo shift: it
  patches the module's form resource in memory, so a plain FM8 editor opened in the *same* process as
  an FM8+ instance shows the 11px gap (no "+"). Cosmetic, stock file untouched, and absent when only
  one of the two is loaded.

- The **overlay button** is a Win32 child over the editor. If a host's GL surface repaints over it in
  some DAW, the fallback is the INI plus the native NGL menu (a later upgrade, addresses already
  located in `docs/hooks.md`).
- **Standalone morph** applies via the internal setter using the EditBuffer captured from the arp
  dispatch, which runs every block; if a future FM8 build gates that call, capture the EditBuffer
  from another per-block site instead.
- Arp-out timing carries the in-block sample offset in VST2/VST3; the standalone WinMM path sends at
  the block boundary (one block of jitter), which is fine for loopMIDI or hardware.
