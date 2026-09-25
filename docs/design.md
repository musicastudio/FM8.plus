# FM8.plus design

This is the original design note for the first two features. The shipped version extends it:
the morph maps any MIDI CC (not just the mod wheel), and three more features were added, Tempo
Override, Increase Gain and GUI Scale. See the README for the current feature set; the mechanisms below still
describe how the morph and arp paths work.

**Attachment model changed since this note.** The original plan (below) renamed each stock module to
`FM8.plus.core` and dropped a same-named proxy in its place, and sideloaded `version.dll` next to
`FM8.exe`. The shipped version never touches a stock file: FM8.plus installs as its OWN files
(`FM8.plus.dll`, `FM8.plus.vst3`, `FM8.plus.exe`) beside FM8, the VST wrappers load the untouched
stock module in place and present a distinct "FM8.plus" identity (feature hooks gated to our instances),
and the standalone is a launcher that injects `FM8.plus.dll` into `FM8.exe` at startup. This is what
makes a Native Access reinstall safe; see the README "How it works" and the `fm8plus-vst-reinstall-resilience`
note. The hook/arp/morph internals below are unchanged.

Features added to Native Instruments FM8 (build 2022-12-23) across the standalone `FM8.exe`, the
64-bit VST2 `FM8.dll`, and `FM8.vst3`:

1. A toggle that maps a chosen MIDI CC (originally the mod wheel, CC1) to a rotation of the Morph
   square handle.
2. A toggle with three arpeggiator modes: Internal (stock), Clone to MIDI (play internally
   and send the arp notes to the plugin MIDI output), MIDI only (send only, FM8 silent).
3. Tempo Override: scale the host tempo the arp follows (0.25x to 4x, or Custom).
4. Increase Gain: extra output gain, up to +10 dB.
5. GUI Scale: 1x to 4x on the whole FM8 GUI, in the standalone and both plug-in formats.

The binaries never receive another update, so every internal function sits at a fixed RVA
forever. That is what makes an in-process hook layer, rather than a rebuild, the right tool.

## Decision summary

One shared feature core plus three thin per-host shims that double as the attach vector. The
core holds all feature logic; only the last stage of MIDI out and the source of CC1 are
host-specific. Decisions that were not obvious, with the reason each won:

- **VST3 morph uses the parameter path, not the CC handler.** The headless probe confirmed
  `IMidiMapping` maps CC1 on channel 0 to parameter id `0x6d69646b`, so in VST3 the mod wheel
  never reaches the internal `FM8Midi` handler as a MIDI event. A single CC-handler hook would
  silently do nothing in VST3. The VST3 process wrapper reads `data.inputParameterChanges` for
  that id instead.
- **The VST3 event output bus is added, not assumed.** The probe confirmed FM8.vst3 registers
  no event output bus (only `Out 1` audio and a 16-channel `Event Input`). We add one by
  patching the component vtable (`getBusCount`, `getBusInfo`, `activateBus`) at the first
  `createInstance`, before the host's first bus query. A full COM aggregation proxy was
  rejected as the largest, most host-variable surface with no graceful fallback.
- **The standalone attaches by a `version.dll` sideload,** not a `winmm` proxy (180 exports)
  and not `CreateRemoteThread` injection (AV-flagged, forces a custom launcher). FM8.exe imports
  only four `version.dll` functions and `version.dll` is not a KnownDLL, so the forward surface
  is four stubs and the user keeps their normal FM8.exe shortcut.
- **No VST2 `canDo` override.** The probe showed stock FM8 already answers `sendVstMidiEvent`
  with 1 and already emits CC7 and CC10 through `audioMasterProcessEvents` on the first block, so
  the arp events feed the existing, proven path.
- **Per-instance state via a chunk trailer.** Arp mode and morph toggle travel per track with
  the DAW project, which is what lets one FM8 drive MIDI out as an arp source while a second plays
  internally. The probe confirmed stock FM8 ignores trailing bytes on `effSetChunk` (it parses by
  an internal length prefix), so the trailer is uninstall-safe. Global INI is the fallback.
- **Validation discipline built in.** TimeDateStamp `0x63a57e00` plus SizeOfImage plus a
  per-site prologue-byte signature, an SEH-guarded install, and degrade to transparent
  pass-through on any mismatch, so a Native Access repair or an unexpected build reverts to stock
  FM8 rather than crashing.
- **Glitch-free modes via note-state bitsets.** Two 128-bit-per-channel masks, with an
  all-notes-off flush on mode change, editor close, and transport stop.

## Components

| Component | Role | ~LOC |
|-----------|------|-----:|
| `core/identity.*` | Resolve module base under ASLR; validate TimeDateStamp, SizeOfImage, per-site prologue bytes; pick the RVA row; SEH-guarded MinHook install; disable a hook and pass through on any mismatch | 200 |
| `core/rvas.h` | Per-binary RVAs and prologue signatures for the internal functions; emitted once by `tools/find_rvas.py`, never changes | 90 |
| `core/morph.*` | CC1 (0..127) into an angle, then centre + r*(cos,sin); calls the internal Morph X/Y setter; radius and start angle from settings; toggle-gated | 70 |
| `core/arp.*` | Per-instance atomic mode byte; two 128-bit-per-channel note masks; fixed 256-entry lock-free event ring written on the audio thread; routing, suppression, flush | 210 |
| `core/midiout.*` | One sink interface, three backends draining the ring: VST2 `VstEvents` into `audioMasterProcessEvents`, VST3 `IEventList::addEvent` into `outputEvents`, EXE `midiOutShortMsg` | 190 |
| `core/settings.*` | `%ProgramData%\FM8.plus\FM8.plus.ini` read and write (machine-wide, one-time carry-over from the old `%APPDATA%` INI); 12-byte chunk-trailer codec; std::atomic mirrors read on the audio thread | 150 |
| `core/ui.*` | Small Win32 child control over the editor: mod-wheel-morph checkbox, three-way arp radio, standalone port picker; per instance via the editor HWND | 220 |
| `shim_vst2` builds `FM8.dll` | Proxy: export `VSTPluginMain` and `NICreatePlugInInstance`, LoadLibrary the renamed core, capture `audioMaster`, install hooks, wrap `dispatcher` (chunk, editor open) and `processReplacing` (flush ring) | 160 |
| `shim_vst3` builds `FM8.vst3` | Proxy: forward `GetPluginFactory`/`InitDll`/`ExitDll`; on first `createInstance` patch the component vtable and hook `getState`/`setState` and `IPlugView::attached` | 180 |
| `shim_exe` builds `version.dll` | Sideload: forward the four imported `version.dll` functions to `System32\version.dll`; install hooks in DllMain before WinMain; own the WinMM output port | 120 |
| `installer.iss` | Inno Setup: locate, verify hash, rename originals to `.core`, drop shims, write INI; uninstall reverses | 90 |
| `tools/find_rvas.py` | pyghidra: resolve the internal functions by RTTI and string anchors in each project, emit `rvas.h`; run once | 100 |
| `CMakeLists.txt` | Core static lib plus three shim targets; vendored MinHook and vst3 pluginterfaces | 60 |

## Per-host attachment

### EXE (FM8.exe)

Sideload `version.dll` into the FM8 install folder. FM8.exe imports only
`GetFileVersionInfoSizeW`, `GetFileVersionInfoW`, `VerQueryValueW`, and `VerQueryValueA`, and
`version.dll` is not a KnownDLL, so the application directory wins the search order and four jump
stubs resolved from `System32\version.dll` satisfy every real call. No byte of FM8.exe changes and
the user's normal shortcut keeps working. Hooks install from DllMain, which runs before WinMain,
so MinHook's thread freeze is trivially safe since no thread is inside FM8 code yet. Single
instance, so state is the INI. MIDI out is our own `midiOutOpen` port chosen in the UI, driven by
`midiOutShortMsg` from the arp hook. Uninstall is deleting `version.dll`.

### VST2 (FM8.dll)

Rename the original in place to `FM8.plus.core` (no `.dll` extension, so scanners ignore it and no
duplicate uniqueID appears) and drop the proxy under the name `FM8.dll`. `VSTPluginMain`
LoadLibrarys the core by absolute path, captures the host `audioMaster`, installs the internal
hooks, calls the core `VSTPluginMain`, and returns the real `AEffect` unchanged (uniqueID
`0x4e696638`, 1094 params, flags 0x139), so existing projects load and gain the features.
`NICreatePlugInInstance` is forwarded so the stock `FM8 FX.dll` shim keeps resolving. We wrap
`dispatcher` only for `effGetChunk`/`effSetChunk` (trailer) and `effEditOpen` (HWND into the
instance map), and wrap `processReplacing` to flush the arp ring after the real call.

### VST3 (FM8.vst3)

Rename the single-file module to `FM8.plus.core`, drop the proxy as `FM8.vst3`. The proxy forwards
`GetPluginFactory`/`InitDll`/`ExitDll`; class ids, parameters, and the state stream are unchanged,
so projects load. On the first `createInstance` we patch the returned component's vtable, shared
across instances so patched once: `getBusCount(kEvent, kOutput)` returns 1, `getBusInfo` describes
an "FM8.plus Arp Out" event bus, `activateBus` records the active flag per component pointer, and
`process` stashes `data` in a thread-local, reads `inputParameterChanges` for param id
`0x6d69646b` (mod wheel), runs the real process, then drains the ring into `data.outputEvents`. We
also hook `getState`/`setState` for the trailer and `IPlugView::attached` for the editor HWND.
Hosts that cache bus layouts need one rescan after install, documented in the installer.

## Toggle UI and persistence

**UI.** FM8's own "FM8" wordmark, widened and with a "+" drawn into it, opens a `TrackPopupMenu` of
checkable items (docs/gui.md 7). The button is a resource FM8 draws, so it needs no window of ours
over the editor and cannot be covered by anything the host has open; only the click is ours, taken by
a subclass on the window FM8 draws into and therefore bound to one editor, one instance. Live feedback
is FM8 itself: the morph handle visibly circles the square under CC1, and in MIDI-only mode the arp
page keeps animating while the routed track makes the sound. Swallowing the press costs FM8's About
panel, which the menu gives back by calling FM8's own dialog function. This avoids reverse-engineering
NI's NGL menu API; a native NGL submenu is an optional later upgrade.

**Persistence.** Per instance inside DAW projects via a 12-byte trailer (`FM8PLUS1`, modwheel u8,
arpmode u8, reserved u16) appended in the VST2 `effGetChunk` wrapper and the VST3 `getState` hook,
stripped and applied in `effSetChunk`/`setState` before FM8 parses the stream. Trailer-less
instances, meaning every existing project and every fresh instance, start stock (morph off, arp
Internal), so a project can never load unexpectedly silent. The probe confirmed stock FM8 tolerates
the trailer. Standalone state and global knobs (morph radius, start angle) live in the INI. The
audio thread reads std::atomic mirrors only.

## GUI Scale data path

Nothing is redrawn and no layout is touched. NI::UIA, the Win32 layer under NGL, already converts
between a logical coordinate space and physical pixels by a per-window DPI scale: it multiplies the
size of every window it creates, divides incoming mouse coordinates, multiplies the rectangles it
hands to `InvalidateRect`, and stretches the software DIB onto the window on `WM_PAINT`. FM8 ships
that code switched off. It never calls `SetProcessDpiAwareness`, so `GetDpiForWindow` always answers
96, and one gate byte in the NI::UIA app object keeps every call site on the 1.0 branch regardless.

So the feature is three detours and a number (`docs/hooks.md`, "GUI scale"): the app-object getter
sets the gate byte, the scale getter returns the chosen factor, and the surface-scale getter is held
at 1 so the DIB stays logical and the `StretchDIBits` at the end of the paint does the enlarging. FM8
then sizes its own window, hit-tests the mouse and repaints correctly with no further help, the
"FM8+" wordmark included, since that is one of its own controls. What is left for FM8.plus is the
part FM8 cannot know about: a hosted editor has to tell the host its new size, over
`effEditGetRect` / `IPlugView::getSize` when it opens and `audioMasterSizeWindow` /
`IPlugFrame::resizeView` when the scale changes while it is open. The standalone resizes FM8's own
top-level window itself. The steps are whole numbers because FM8's blit is the only thing enlarging
the artwork: its one `SetStretchBltMode` asks for `HALFTONE`, which interpolates and softens
everything, so FM8.plus swaps that import for `COLORONCOLOR` and each pixel is replicated exactly.

The detours are process-wide, so each plug-in shim registers the editor window the host gave it and
the scale getter answers 1.0 outside that window tree. A plain FM8 instance sharing the module in the
same DAW stays exactly stock.

## Arp MIDI out data path

The arp engine runs inside `process()`/`processReplacing` on the audio thread. The internal hook
H2 sits on the arp module's per-block emit. For each note event it consults the instance mode and
the two 128-bit-per-channel masks `internalOn` and `externalOn`:

- **Internal (stock):** deliver to the sound module, touch no mask, emit nothing. The ring stays
  empty.
- **Clone to MIDI:** on note-on, deliver internally and set `internalOn`, and enqueue to the ring
  and set `externalOn`. On note-off, deliver internally if `internalOn` is set (clear it) and
  enqueue externally if `externalOn` is set (clear it).
- **MIDI only:** on note-on, suppress internal delivery (drop the event from the list the sound
  module reads) and enqueue externally, setting `externalOn`. On note-off, still deliver internally
  if `internalOn` is set from a note started in another mode (clear it), and enqueue externally if
  `externalOn` is set (clear it).

Because note-offs are routed by which mask bit is set, switching mode mid-pattern or crossing a
block boundary never hangs a note on either side. On mode change, editor close, and transport stop
we flush: emit an external note-off for every `externalOn` bit and send internal all-notes-off,
then clear both masks.

**Timing.** H2 writes each event with its in-block sample offset into a fixed-size lock-free ring
(allocation-free, mode is an atomic byte). Sinks drain after the real process:

- **VST2:** the `processReplacing` wrapper builds one `VstEvents` block, `VstMidiEvent.deltaFrames`
  = ring offset, and calls the captured `audioMaster(effect, audioMasterProcessEvents, ...)`.
- **VST3:** the `process` wrapper calls `IEventList::addEvent` into the stashed
  `data.outputEvents` with `Event.sampleOffset` = ring offset.
- **EXE:** the arp hook calls `midiOutShortMsg` immediately; events leave at the block boundary,
  one block of jitter, fine for loopMIDI or hardware. No timer-scheduled variant is built.

If the in-block offset cannot be recovered at the hook site, fall back to offset 0 (block-boundary
timing) rather than adding complexity.

## Mod wheel morph data path

One sink, two sources. The sink is the internal Morph X/Y setter (the `MorphX`/`MorphY`
variable-properties path automation uses), called, not hooked, so the on-screen `XYHandleMorph`
follows and there is one morph code path across hosts. Math: `theta = start + (v/127)*2pi`,
default `start = -90 deg` so CC 0 sits at the top, `x = 0.5 + r*cos(theta)`,
`y = 0.5 + r*sin(theta)`, clamped to `[0,1]`, `r` from the INI (default 0.5). The chosen CC is
swallowed, never reaching FM8, so it drives the morph only: no mod-wheel movement, no MIDI-learn.
In VST2 and EXE the handler detour returns without calling the original; in VST3 the process wrapper
hides that parameter's queue from FM8 for the block.

Sources differ by host since CC1 arrives differently:

- **VST2 and EXE:** hook H1, the `FM8Midi` control-change handler, catch CC1 there, the same
  internal function at a different RVA.
- **VST3:** CC1 does not reach `FM8Midi`; `IMidiMapping` converts it to a parameter change on id
  `0x6d69646b`. The process wrapper reads `data.inputParameterChanges` for that id, takes the last
  point's normalized value as the 0..1 wheel position, and drives the morph setter. No H1 hook is
  used in VST3.

If the internal setter proves unsafe to call from the audio thread, fall back to
`setParameter(21, x)`/`setParameter(22, y)` in VST2 and `outputParameterChanges` for ids 21/22 in
VST3; the EXE has no external param API, so it depends on the internal setter being callable.

## Hook points to find in Ghidra (checklist)

- [ ] **H1. `FM8Midi` control-change handler** (exe and vst2 only). Argument layout (this,
  channel, cc, value, or `MidiEvent&`); confirm CC1 passes through it. Not needed for VST3.
- [ ] **H2. Arp per-block emit** (`NI::K2IMPORT::DESC::MIDI_ArpeggiatorModule::process` or the
  `MIDIArpeggiator_Engine` step/emit it wraps) in exe, vst2, vst3. Need the signature and
  block-length arg; the `MidiEvent` field layout (status, data1, data2, channel, sample or tick
  offset) in `MidiEventArray`/`BeatTickTimeEventList<MidiEvent>`; the list API to drop a note event
  for MIDI-only; the `Arp On` flag location in `MIDIArpeggiator_DataContainer`; whether split-bass
  pass-through notes travel through the same list.
- [ ] **Morph X/Y setter** (`MorphX`/`MorphYVariableProperties` path) in exe, vst2, vst3. Called,
  not hooked. Confirm the value domain (0..1, centre 0.5), that it moves `XYHandleMorph`, what
  `MorphByAutomation` gates, and whether it is audio-thread safe.
- [ ] **Per-binary identity:** SizeOfImage and the prologue bytes at each hook site for the
  signature guard (TimeDateStamp `0x63a57e00` already known).
- [ ] **VST3 confirmation:** that `process` runs the arp on the audio thread; that param id
  `0x6d69646b` appears in `inputParameterChanges` when the mod wheel moves; the component vtable
  slot layout for `getBusCount`/`getBusInfo`/`activateBus`/`process`, checked against the vendored
  pluginterfaces.
- [ ] **Self-path check:** any `GetModuleHandleW(L"FM8.dll")` or `GetModuleFileNameW` that depends
  on the module's filename, which the rename to `FM8.plus.core` would break. Strings show only
  export-directory names; confirm via xrefs.

## Risks and mitigations

- **VST2 host may not route plugin MIDI.** Some hosts ignore plugin-originated events, so
  Clone/MIDI-only produce nothing routable there and MIDI-only leaves FM8 silent. Mitigated by
  honest labelling ("MIDI only (FM8 silent)") and by the live arp-page animation as confirmation.
- **VST3 output bus caching.** Some hosts cache bus layouts and need one rescan after install; the
  installer's final page says so.
- **Morph setter thread safety.** Target the automation-path variable setter, not the GUI handle,
  since automation already drives it off the audio path. Param-set fallback if unsafe.
- **Wrong build or Native Access repair.** TimeDateStamp, SizeOfImage, and prologue signature; an
  SEH-guarded install; degrade to pass-through; the proxy verifies the core hash and falls back to
  plain forwarding; the installer is idempotent.
- **Module rename breaks a self-path lookup.** Verify no literal `"FM8.dll"` self-lookup before
  shipping.
- **AV or SmartScreen on unsigned proxies.** Sign the binaries and installer if a certificate is
  available; `version.dll` forwarding is far less heuristically suspicious than code injection.
- **Testing caveat.** A process that has loaded FM8.vst3 or FM8.dll never fully exits (teardown
  blocks in kernel), so headless probe runs hard-terminate and may leave zombies.

## Build and install

**Build.** CMake and MSVC 2022, x64. One `core` static library plus three shim targets: `FM8.dll`,
`FM8.vst3`, `version.dll`. `version.dll` uses a `.def` with four forward stubs resolved from
`System32\version.dll` at load. MinHook and the VST3 pluginterfaces headers are vendored.
`tools/find_rvas.py` runs once against the three Ghidra projects to emit `core/rvas.h`; the
binaries never change, so the RVAs are stable forever.

**Install (Inno Setup, `FM8.plus-Windows-Installer.exe`).** Locate the three FM8 files via
`HKLM\SOFTWARE\Native Instruments\FM8`, then default paths, then a browse dialog. Verify each
original's SHA-256 and TimeDateStamp `0x63a57e00`. Rename `FM8.dll` and `FM8.vst3` in place to
`FM8.plus.core`, copy the two proxies under the original names, copy `version.dll` next to
`FM8.exe`, write the INI defaults, and prompt the user to rescan plugins. Normal-user install with
one elevation for the Common Files VST3 path. Idempotent on re-run after a Native Access repair.
Uninstall reverses the renames, deletes `version.dll` and the INI.
