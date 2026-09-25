# DXi

FM8.plus brings back the DXi that Native Instruments dropped after FM8 1.0.3. This is what reverse engineering 1.0.3's own DXi found, and how FM8.plus's DXi follows it.

## Which FM8 had a DXi

NI's FM8 spec sheet listed "Stand-alone, Audio Units, VST, DXi, RTAS" from December 2006 to at least January 2009. The FM8 1.0.4 Getting Started guide (product version 1.0.4, August 2009), the release that added x64, lists only VSTi, Audio Unit and RTAS, and the September 2010 spec sheet has no DXi. The 1.0.3 installer (a Wise installer, 2007-11-09) confirms it: its script installs `%MAINDIR%\DXi\FM8DXi.dll` beside the VST2 `FM8.dll`, both FM8 1.0.3.003 built 25 seconds apart on 2007-10-18.

FM8 1.4.1's 32-bit VST2 still carries traces: its host detection compares the host's product string with `"NIHOST"` and then `"NI-DXi"` (`FUN_1002fe70`), and its install scanner reads an `InstallDXiDir` value (`FUN_102f7240`). There is no DXi code in it to switch back on.

## What NI's DXi is

`FM8DXi.dll` is a full FM8 build with NI's generic DXi layer compiled in, not an adapter around the VST2. That layer is Cakewalk's DXi2 SDK framework (a DirectShow filter, not a DMO) on Microsoft's DirectShow BaseClasses, plus NI's `DXi2Wrapper`. Addresses are in the 1.0.3 DXi (`FM8_103_DXi`, image base `0x10000000`).

| Piece | Where | Notes |
|---|---|---|
| Class factory table | `0x10dea8e4`, count `0x10dea90c` | "NI Plug-In" `{A4615A6D-B05D-4D25-B6B2-335C72DCC108}`, "NI Plug-In Properties" `{79EA4DF0-EF7E-4085-9F98-A56A06C1579B}` |
| `DllRegisterServer` | `0x100356b4` | `AMovieDllRegisterServer2`, the filter's `IAMovieSetup::Register`, then `HKCR\MfxSoftSynths\{clsid}` with `Description`, `HelpFilePath`, `HelpFileTopic` |
| COM object | 0x348 bytes, `new` at `0x104cada0` | `+4` `IMfxSoftSynth`/`IMfxSoftSynth2`, `+8` `IMfxNotify`, `+0xc` `IMfxInputPort`, `+0x1e4` `IBaseFilter`, `+0x1e8` `IPersist`/`IAMovieSetup`, `+0x228` `ISpecifyPropertyPages`, `+0x22c` `IDispatch`, `+0x230` `IPersistStream`; the object at `+0x14c` answers `IMediaParamInfo`, `IMediaParams` and Cakewalk's `IMediaParamsUICallback`/`IMediaParamsSetUICallback` |
| `CDXi` implementation | `+0x108`, vtable `0x10f51060` | Initialize `0x104ddb78`, Process `0x104de6b4`, Allocate/FreeResources `0x104e056c`/`0x104e0634`, PersistGetSize/Load/Save `0x104edcc4`/`0x104edd20`/`0x104eddec` |
| `IMfxSoftSynth::Connect` | `0x104ce59c` | Queries the tempo map, time converter, meter map, input callback and notify host, then always `Release`s the context |

## Why FM8.plus drives FM8 through the VST2

NI's `Process` deinterleaves the host buffers, turns the MIDI queue into FM8's internal events (`FUN_104e424c(time, status, data1, data2)`), and calls the render helper `FUN_104e5ba0(in, out, frames)`. That helper points the plug-in's port list (`plugin+0x230`) at the buffers and calls the framework renderer (`FUN_104bd4e4`, then `FUN_104b3cd4`).

The 1.0.3 VST2 does the same thing in `processReplacing` (`0x104be038`). It fills the same port list and calls the same renderer, which is byte-identical in both builds (`FM8_103_VST_32` `0x104b3c20` against the DXi's `0x104b3cd4`). Parameters, MIDI and state follow the same pattern: the DXi plug-in class (ctor `0x10035d30`, vtable `0x10e69a40`) shares its first 20 virtual slots with the VST2's internal interface at `+0xb0` (vtable `0x10e415c0`), and its 7 extra DXi slots forward to the engine at `plugin+0xac`.

So NI's DXi and VST2 adapters are thin layers over one shared framework path, and in 1.4.1 `AEffect::processReplacing` is that path. Calling the framework directly instead would depend on private layouts that drifted between 1.0.3 and 1.4.1 (init moved from slot `+4` to `+8`, the engine pointer from `+0xac` to `+0x214`, the engine grew from `0x5350` to `0x5490` bytes) for no gain.

## FM8.plus's DXi

`src/dxi/dxi.cpp`, built into the 32-bit `FM8.plus.dll`, which exports both `VSTPluginMain` and the COM entry points.

- **Filter.** One input pin clocks the synth with the host's buffers and one output pin returns the same format (32-bit float or 16-bit PCM, any channel count, FM8's stereo mixed or spread to fit). Each buffer is rendered in chunks of up to 1024 frames.
- **MIDI.** Playback events arrive through `OnEvents` in host ticks and are converted to sample times through the host's time converter, as the SDK does. A streamed note carries its own duration, so it becomes a note-on and a timed note-off. Live input through `OnInput` plays at the start of the next chunk. SONAR's per-track forced channel, Key+/Vel+ and mute arrive through `IMfxNotify` and are applied. A transport stop releases every held note.
- **FM8.** Each DXi instance calls this module's own `VSTPluginMain`, so it gets FM8.plus's wrapped `AEffect` with every feature, and answers its host callback from the MFX tempo map (tempo, position, transport).
- **State.** `IPersistStream` saves FM8's chunk, with the FM8.plus settings trailer.
- **Editor.** The property page is a child window in the host's property frame, with FM8's editor opened inside it and idled on a timer.
- **Registration.** `DllRegisterServer` does what 1.0.3's did: DirectShow registration, then `HKCR\MfxSoftSynths\{E49B1FBE-A4A2-42EE-9530-D21552A4EC1E}`. The installer registers it from `%ProgramData%\FM8.plus`, beside a copy of the user's own 32-bit FM8 1.4.1 `FM8.dll`.

**GUI Scale.** Changing the scale with the editor open grows FM8's window, but neither DXi, MFX nor `IPropertyPage` has a way for a page to ask its frame for more room: the DXi SDK's sample host sizes a page once from `GetPageInfo`, `IMfxNotifyHost` only carries audio-port changes, and SONAR remembers a plug-in window's size from its first opening. So the page does what JUCE's VST wrapper does for hosts that ignore `sizeWindow`: it resizes each window above it by the same amount, keeping its margins, up to the top-level DX window, stopping at an MDI client or before a parent holding over 100 px of other content. Reopening the editor sizes it from `GetPageInfo`, which answers at the current scale. Dragging SONAR's window bigger also exposed a bug in FM8.plus's 1.4.1 scaling layer, which is not DXi-specific: it scaled FM8's own `InvalidateRect` calls but not the paint rect Windows hands FM8 in `BeginPaint`, so any partial repaint the host asked for (a strip exposed by dragging one edge) was redrawn in the wrong place at double size and the strip itself left blank. `BeginPaint` is now converted too. The harness's `--drag` option reproduces it at 2x (the frame starts at half size and is dragged out one edge at a time).

Not implemented yet: `IMediaParams` automation of FM8's parameters from the host, and patch and note names.

Not possible: arp MIDI out. The DXi still captures the arpeggiator's notes (it renders through the same detours as the 32-bit VST2), but a DXi has no way to hand MIDI back to the host. `IMfxSoftSynth::OnEvents` only receives events, and the SDK describes the synth as always the last element in the chain; only a MIDI effect (`IMfxEventFilter`) gets an output queue. This is a limit of every DXi, not of FM8.plus. SONAR X1 and later take MIDI output from VST instruments, so the 32-bit VST2 FM8.plus is the way to get arp MIDI out there.

## Testing

`build32\Release\dxihost.exe <FM8.plus.dll>` builds the graph a DXi host builds (silence source, the synth, a capture renderer) with the DirectShow filter graph manager, and drives it through MFX with a 120 BPM, 960 PPQ context. It checks that a streamed note sounds and ends, that no events give silence, that live input sounds, that state saves (with the FM8.plus trailer) and loads, and that the property page opens FM8's editor.

`dxihost.exe <FM8.plus.dll> --view` is the visual harness. It opens the editor the way SONAR does, in a frame of its own with a strip above the page, runs FM8 live into DirectSound, and plays whatever arrives on every MIDI input, plus a Chord button, a Reopen button and a Screenshot button. `--ole` uses the stock OLE property frame instead, and `--shot out.bmp --after 4` plays a chord, prints the window tree, saves a screenshot of the harness window and exits. The harness found that property frames ask a page for its size before handing it its object, which left the page at a fallback 800x600 (FM8's editor clipped on the right, an unpainted band below); the page now answers from the newest instance, and paints its own background.

In SONAR the editor came up mostly blank: only what FM8 redraws on its own (the browser lists, the morph square, the meters) appeared. The page's diagnostic log (create `%ProgramData%\FM8.plus\dxi.log` and the page appends what the host does and what reaches its windows) showed why. SONAR activates and shows the page while its DX window has redraw switched off (`WM_SETREDRAW`, which clears the window's visible bit), then turns redraw back on and repaints only itself, so FM8's editor never gets a full paint. The page now repaints the whole editor each time it actually comes on screen, checked on its idle timer. SONAR's DX window is a frame with a host area inside it (holding the Presets strip and the page), and neither clips its children, so as a guard the page also gives every window above it, up to that frame, `WS_CLIPCHILDREN` while it is active, and takes it back on Deactivate. The harness's SONAR-style frame reproduces both the window tree and the redraw-off open, and its Host repaint button repaints the frame and host area alone.

## Reference

Cakewalk's DXi2 SDK (interface headers, a sample synth and a sample host) is on GitHub as `firodj/dxi2_sdk`. It is Cakewalk's, "All rights reserved", so it served as reference only; `src/dxi/mfx.h` declares just the interface IDs, method order and struct layouts the ABI needs.
