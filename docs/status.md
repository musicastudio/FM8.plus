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

**Editor overlay (the "+" button), VST2 and VST3, in a real window** (`tools/vsteditor.py`, a
ctypes GUI host that opens the editor in a top-level window, dumps the child-window tree with Z order
and positions, and screenshots just that window). First report from Reaper (2026-09-10): the logo
shifted but no "+" in either format. The harness found three separate causes, all fixed:

| Cause | Symptom in the harness | Fix |
|-------|------------------------|-----|
| `UpdateLayeredWindow` `pptDst` is parent-relative for a child window; the shim passed the screen rect | overlay created at (109,22) but found at (109+editorX, 22+editorY) | pass `pptDst = nullptr` |
| FM8's full-size `NIVSTChildWindow` sits above a newly created sibling either way | overlay directly below FM8's child in Z order from the moment the editor opens | explicit `SetWindowPos(HWND_TOP)` after creation (a single raise sticks; FM8 never re-raises) |
| The VST3 shim had no editor hook at all | no `FM8plusOverlay` window in the process | hook `IEditController::createView` and the view's `attached`/`removed` from the live vtables, gated to FM8+ instances |

After the fixes both formats show `FM8plusOverlay > NIVSTChildWindow` at (109,22) right after the
editor opens, and the screenshot shows the "+" beside the shifted logo with no intervention.

The "+" position itself was then found to differ from the standalone (2026-09-11): the plug-in
overlay was created at client (109,22) while the EXE's floated at window-rect (109,82), which is
client (101,31) under the Windows 10 frame, so the plug-in "+" sat 8 px right and 9 px high of the
EXE's. Both hosts now share one client-relative origin (`kPlusX`/`kPlusY` in `ui.cpp`; the
standalone glues through `ClientToScreen` instead of the window rect). Harness screenshots put the
"+" pixel box at (111,41)-(124,54) in VST2, VST3 and the EXE, against logo text rows 35-57.

**GUI analysis and layout tooling** (`docs/gui.md` and its three sub-documents, `tools/fm8gui.py`,
`tools/frm_grammar.py`, `tools/gen_forms.py`, 2026-09-10). FM8's GUI is 74 `FRM` form resources
built from 13 NGL control classes; the byte grammar of every class, the picture (PNG/TGA), font
(picture-font strips, TrueType) and manifest formats, and the runtime (form classes, events,
parameter links, software rendering) are documented from the decompilation.

| Check | Result |
|-------|--------|
| Decompile every FRM to typed XML and recompile | pass (74/74 byte-exact) |
| Previewer reconstructs the editor from form data (pictures, 9-slice panels, picture-font captions) | pass (`build/gui/preview/3.png` matches the real editor) |
| Rebuilt header forms served to FM8 through its own import table (`Rsrc`), replacing the byte patch | pass (VST2 and VST3: `FM8.dll!FindResourceA -> FM8.plus.dll`, wordmark shifted, "+" beside it) |
| VST2 feature regression after the shim change | pass (same numbers as above) |

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
The FM8+ shortcut icon is FM8's own icon with our "+" over it, so it is composited at install time by
`tools\make_icon.ps1` from the user's own FM8.exe; only `installer\icon_overlay.ico` (our artwork)
ships. Nothing Native Instruments produced is redistributed in the repo, the binaries, or the setup.

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

- The **overlay button** is a Win32 child over the editor, raised once above FM8's child at creation.
  If some DAW re-raises the plug-in window later, the "+" would vanish there; the cheap upgrade is a
  periodic re-raise timer (the standalone already has one), and the deeper fallback is the INI plus
  the native NGL menu (addresses already located in `docs/hooks.md`). Run
  `python tools/vsteditor.py vst2|vst3 [path]` after any overlay or shim change.
- **Standalone morph** applies via the internal setter using the EditBuffer captured from the arp
  dispatch, which runs every block; if a future FM8 build gates that call, capture the EditBuffer
  from another per-block site instead.
- Arp-out timing carries the in-block sample offset in VST2/VST3; the standalone WinMM path sends at
  the block boundary (one block of jitter), which is fine for loopMIDI or hardware.
