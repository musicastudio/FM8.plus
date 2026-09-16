# Status

What is verified, and what still needs a real DAW. Honest accounting, no overclaiming.

## Verified

**Reverse engineering.** All three binaries decompiled (Ghidra 12.1.2), every hook located,
adversarially verified, and cross-ported. Each hooked function resolves at its recorded address with
a byte-identical size across exe / vst2 / vst3 (see `tools/q.py` and the check in the build history).

**VST2, end to end against the installed FM8** (the FM8.plus wrapper loading stock FM8.dll from its own
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
factory to present FM8's two audio classes under distinct FM8.plus identities, so it coexists with stock
FM8. Instances created through our factory get the added event-output bus (stock FM8 exposes none);
plain FM8 instances are left alone.

```
factory classes: 2
  [0] Audio Module Class  FM8.plus     cid=ce54...6669...  vendor='Native Instruments GmbH / musica.studio'
                                        (stock FM8 is 4e54...6669..., vendor 'Native Instruments GmbH')
  [1] Audio Module Class  FM8 FX+  cid=ce54...8669...
event out: 1 bus(es)
    [0] 'FM8.plus Arp Out' channels=16 busType=0 flags=0x1
```

**The "FM8+" button, all three hosts, in a real window** (`tools/vsteditor.py` for the plug-ins, a
ctypes GUI host that opens the editor in a top-level window; a matching probe drives the standalone).

The button was a layered child window over the editor until 2026-09-16, and that design cost three
separate fixes to get visible at all (`UpdateLayeredWindow`'s `pptDst` is parent-relative for a child
window; FM8's full-size `NIVSTChildWindow` sits above a newly created sibling either way; the VST3
shim had no editor hook), and it still put a window of ours in the host's Z order, where other
plug-ins and windows could land on top of it. It is now FM8's own artwork instead: `Core::serveForms`
hands FM8 a widened FRM 5/15 and a widened PICTURE 193 with the "+" painted on, and `ui.cpp`
subclasses the window FM8 draws into and takes the click (docs/gui.md 7). Nothing of ours is in any
Z order, and FM8 scales and repaints the "+" with the rest of its GUI.

Verified against the installed FM8 with the shims as they ship:

| Check | VST2 | VST3 | Standalone |
|---|---|---|---|
| FM8 draws the widened wordmark (logo-coloured pixels counted in the 22px plus strip) | pass, 76 | pass, 76 | pass, 76 |
| Posted click on the "+" opens the FM8.plus menu | pass | pass | pass |
| The same at 2x GUI Scale (294 pixels, click still lands) | n/a | n/a | pass |
| "About FM8" brings up FM8's own About panel | pass | untested live | pass (482x338 `#32770`) |
| Window tree under the host's editor HWND | `NIVSTChildWindow` only | `NIVSTChildWindow` only | the top-level window itself, no children |

`SetWindowSubclass` returns 0 when it is called from a thread other than the window's owner, which is
exactly what the standalone did (it finds FM8's window from a worker thread): the wordmark drew but no
click ever arrived. It now rides into FM8's UI thread on a one-shot `WH_CALLWNDPROC` hook. The VST3
About path is the same core code as VST2's with the address from the same decompilation; only a live
run is missing, since the VST3 test host has no `process()` call to capture the pointer with.

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
the same button proven in the earlier `version.dll` build (the **FM8+** wordmark
and the four-submenu menu). Plain `FM8.exe`, launched normally, is untouched.

**GUI Scale, in a real window** (`tools/vsteditor.py`, 2026-09-15). The feature drives NI::UIA's own
HiDPI layer, which FM8 ships switched off (see `docs/hooks.md`, "GUI scale"). Verified against the
installed FM8 with the shims as they ship:

| Check | Result |
|-------|--------|
| VST2 editor at 2x / 3x / 4x | pass (`effEditGetRect` 1896x1124 / 2844x1686 / 3792x2248, FM8's child window and the whole GUI match) |
| VST3 editor at 2x | pass (`IPlugView::getSize` 1896x1124, same render) |
| Standalone at 2x | pass (window 1912x1183 from startup, scaled before FM8 creates it; keyboard strip and all pages render) |
| Mouse lands on the control under the pointer at 2x | pass (clicks at `logical x 2` switch the Navigator page they name; at 1:1 those points are in the keyboard strip) |
| Scale changed from the menu while the editor is open | pass (FM8's child 948x562 -> 2370x1405, host told the new rect, full repaint, INI updated) |
| The "+" tracks the scale | pass (it is part of the wordmark bitmap, so FM8's own blit scales it; the click rect follows) |
| Enlarging is pixel-exact, not interpolated | pass (3x standalone: every source pixel is a hard 3x3 block; FM8's `SetStretchBltMode(HALFTONE)` import is answered with `COLORONCOLOR`) |
| VST2 feature regression at 1x | pass (arp, morph, tempo and gain numbers unchanged) |

**Installer**: `installer\FM8.plus.iss` (Inno Setup) and `install.ps1` / `uninstall.ps1` install
FM8.plus as its own files beside stock FM8 (`FM8.plus.dll` in the VST2 folder, `FM8.plus.vst3` in the
VST3 folder, `FM8.plus.exe` + `FM8.plus.dll` beside `FM8.exe`) plus desktop and Start Menu shortcuts.
No stock file is renamed, copied, or modified, so uninstall just removes the added files. The Inno
installer compiles clean and the PE-timestamp build gate is verified against the installed binaries.
The FM8.plus shortcut icon is FM8's own icon with our "+" over it, so it is composited at install time by
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
- **Standalone** button, menu and About panel confirmed on a real run (see above); the arp-out to a WinMM
  port and the actual audio still need a listening test with a MIDI monitor.
- **VST2 host coverage.** Plugin-to-host MIDI is not routed by every host; where a host drops it,
  Clone/MIDI-only produce nothing downstream (the mode is labelled honestly).
- **GUI Scale live resize in a DAW.** Changing the scale while the editor is open asks the host to
  resize it (`audioMasterSizeWindow` / `IPlugFrame::resizeView`); a host that declines leaves its own
  window at the old size until the editor is reopened, which always picks the new scale up. Which
  hosts decline is untested.

## Known limitations / follow-ups

- **Coexistence with plain FM8 in one process.** FM8.plus loads the stock FM8 module in place and
  shares it, so the feature hooks are gated to our instances (by `Core::current` in VST2, by an instance
  registry in VST3) and plain FM8 stays stock. The one shared side effect is the logo shift: it
  serves the header forms and the wordmark bitmap to the shared module, so a plain FM8 editor opened
  in the *same* process as an FM8.plus instance also shows the "FM8+" wordmark. Its clicks are untouched
  (no subclass on that window), so the logo still opens FM8's About panel there. Cosmetic, stock file
  untouched, and absent when only one of the two is loaded.

- **"About FM8" needs the audio thread to have run once.** The FM8App pointer is captured from the arp
  dispatch, which runs every block, so the item is live within a block of instantiation; until then it
  is greyed rather than guessing a pointer. Run `python tools/vsteditor.py vst2|vst3 [path]` after any
  change to the button or the shims.
- **Standalone morph** applies via the internal setter using the EditBuffer captured from the arp
  dispatch, which runs every block; if a future FM8 build gates that call, capture the EditBuffer
  from another per-block site instead.
- Arp-out timing carries the in-block sample offset in VST2/VST3; the standalone WinMM path sends at
  the block boundary (one block of jitter), which is fine for loopMIDI or hardware.
