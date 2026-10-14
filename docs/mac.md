# FM8.plus on macOS

FM8.plus for FM8 1.4.6 on the Mac comes as three plug-ins beside NI's own: `FM8.plus.vst` (VST2, Intel only, since NI's FM8.vst is Intel only), `FM8.plus.vst3` and `FM8.plus.component` (Audio Unit), both universal. There is no Mac standalone: FM8.app is built with the hardened runtime and library validation, so nothing can be loaded into it, and FM8.plus is not going to ship a host application of its own. AAX is not offered either, since it needs Avid's SDK and PACE signing.

Every feature from Windows is there: Morph Rotate Control, Arpeggiator MIDI out (Internal, Clone to MIDI, MIDI only), Tempo Override, Increase Gain, GUI Scale, and the "FM8+" wordmark menu with About FM8. Settings live in `/Users/Shared/FM8.plus/FM8.plus.ini`, with the same keys as the Windows INI. Per-instance settings travel with the project in the VST2 chunk and in the AU's class info.

## How it reaches FM8

NI ships the Mac binaries with their full C++ symbol table, so nothing here is an address. `src/mac/machook.cpp` finds FM8's image with `dladdr` and resolves every target by its mangled name from the symbol table in FM8's `__LINKEDIT`. One table covers the VST2, VST3 and AU builds on both architectures, and a build missing any symbol is left stock.

From least to most invasive:

| What | How | Where |
|---|---|---|
| "FM8+" wordmark | `NI::UIA::ResourceFacade::getResource` looks in the in-memory `g_theResourceMap` (a libc++ `std::map<fourcc, std::map<id, {data, size}>*>`) before it opens `FM8.rsrc`. FM8.plus inserts the widened `FRM ` 5 and 15 (the wordmark rect is four big-endian int32s on the Mac) and a rebuilt `XTGA` 193 bitmap, made at runtime from the user's own FM8.rsrc. Re-served before each editor opens, since FM8 empties the map when its last instance closes. | `core_mac.cpp` serveLogoMac |
| Logo click | `FormMain::onControlEvent` is virtual; both of its vtable entries (the primary one and the `Thn608` thunk) point at a detour that opens the menu for control 5, notify 0x186a1, when the form belongs to a bound FM8.plus instance. | `core_mac.cpp` hookControlEvents |
| Instance binding | The FM8 engine object is found by walking pointers from the host wrapper's object and recognising FM8's vtable by the typeinfo pointer that precedes it. | `core_mac.cpp` bindInstance |
| Morph | The morph CC is taken at the plug-in's MIDI input (VST2 events, AU `MIDIEvent`, the VST3 parameter the host maps it to) and applied with `FM8EditBuffer::SetParameter` on tags 0x84/0x85. | shims, `core_mac.cpp` setMorphXY |
| About FM8 | What FormMain does for the wordmark: `FM8::getFormManager()`, its window (vtable +0x130), that window's base (+0x18), then `StandardAboutDialog2::MSVCHelper<FM8AboutDialog>::doShow`. | `core_mac.cpp` showAboutMac |
| VST3 buses, process, editor | Each FM8.plus instance's IComponent, IAudioProcessor, IEditController and IPlugView get their own copy of the vtable with FM8.plus entries in it. Plain FM8 objects keep FM8's tables. | `shim_vst3_mac.mm` |
| GUI Scale | FM8 builds its editor inside a wrapper view whose frame grows by the scale while its bounds stay logical, so Cocoa scales the drawing and maps the mouse back. FM8's own view keeps frame equal to bounds: scaled directly, it repainted partial updates into its unscaled frame and blanked the rest of the editor. The VST3 `onSize` the host sends is divided back to the logical size before FM8 sees it. Steps that would not fit the screen are greyed. Where an FM8 pixel lands on a fractional number of screen pixels (1.25x and 1.75x, and 1.5x off Retina), FM8's one `CGContextDrawImage` of its offscreen picture (`DeviceContextImpl::updateFromPicture`) is redirected through its symbol pointer and the repainted area drawn with sharp bilinear instead, pixel-aligned, as on Windows (`src/core/sharp.h`). The wrapper's frame is rounded up to whole pixels and FM8's view fills it (`macui::scaleWrapper`): 562 rows at 1.25x are 702.5 pixels, and a view that size had Core Animation squeeze its 703-pixel backing into it, softening rows towards the bottom; the picture's top-left is snapped to a whole pixel once, towards the window's corner, so every repaint maps rows the same way. | shims, `ui_mac.mm` scaleWrapper, `core_mac.cpp` drawImage |
| Arp MIDI out | See below. | `machook.cpp`, `core_mac.cpp` arpEvents |

## The arpeggiator

Arp notes can only be told from live ones inside `FM8Midi::processMidiEventsFromMIDIArpeggiator`, which fetches the arp's events for a sample position (`EditBuffer+0x29a0` is the arpeggiator, `getMidiEvents`, count at +8, 0x28-byte elements from +0x10) and hands each to `FM8Midi::processMidiEvent`. Both are only ever called directly, so there is no vtable to use.

Rewriting FM8's code to detour them works on Intel in an unhardened host and nowhere else: on Apple Silicon every executable page is signature checked, and hardened-runtime hosts (Logic, `auval`, most current DAWs) kill a process that runs a modified page. So FM8.plus modifies nothing. It loads an address inside the function into the CPU's hardware breakpoint registers on each audio thread (`thread_set_state` with the debug state, on the calling thread), and a SIGTRAP handler turns the stopped thread's state into a call of `arpEvents`, which runs FM8's own event loop with the FM8.plus routing in between and returns into FM8's epilogue. Plain FM8 instances that run on an armed thread take the same loop with no routing, which is the stock one.

The address is the first instruction past the function's "no events" exit, not its entry. `FM8Audio::process` calls the dispatch once per sample, so a breakpoint on the entry trapped 48,000 times a second per instance at 48 kHz, each trap a kernel exception plus a signal, and pinned a core (reported on an M4 in REAPER, v1.0.8). Past the exit it traps only on samples where the arp has a note to send. At that instruction the registers still hold `this`, the event array, the thread and the sample position (x86_64 r15, r12, r14, rbx; arm64 x20, x0, x19, x21); `core_mac.cpp` checks the function's bytes against the ones these were read from before arming, and the function is identical in the VST2, AU and VST3 builds apart from call displacements.

Tested with FM8 on an Intel Mac in all three formats, including a host signed with the hardened runtime and Apple's `auval`. `tools/mac/hwbp_test.cpp` tests the redirect itself with no FM8, and CI runs it on every push on Apple Silicon: native arm64, arm64 under the hardened runtime, and x86_64 under Rosetta (where the Intel-only VST2 runs on an Apple Silicon Mac). All three pass.

## Building and testing

`sh tools/mac/build.sh` builds the three bundles and the test hosts into `build-mac/` with the Xcode command line tools, and `sh tools/mac/package.sh` makes the release DMG and zips. The test hosts drive a real FM8 install:

```sh
build-mac/vst2probe ~/Library/Audio/Plug-Ins/VST/FM8.plus.vst --arp
build-mac/vst3probe ~/Library/Audio/Plug-Ins/VST3/FM8.plus.vst3 --arp
sh tools/mac/ingui.sh "build-mac/auprobe --arp"
```

Each also takes `--morph` (with `morph_cc=11` in the INI) and `--editor out.png x y`, which clicks a logical point and reports the menu. `--scale <dir> <start> <sequence>` runs the GUI Scale test in `tools/mac/scaletest.h`: it checks the editor opens at the saved scale (`<start>`, from `gui_scale` in the INI), picks each step of `<sequence>` (e.g. `1,2,3,4,1,3,2,1,2`) from the real menu, checks the view and host sizes and compares every screenshot with 1x, clicks Navigator > Attributes at its scaled position, closes and reopens the editor, and opens About FM8, saving screenshots of each step in `<dir>`. AU hosts must run in the logged-in GUI session, which is what `ingui.sh` is for. `FM8PLUS_TRACE=1` prints what the core resolved and bound.
