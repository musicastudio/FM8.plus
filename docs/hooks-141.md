# FM8 1.4.1 Hook Reference

FM8 1.4.1 (R1599, built 2015-10-20) is the last release Native Instruments shipped with a 32-bit plugin. Everything from 1.4.2 on is 64-bit only, and VST3 did not arrive until 1.4.5, so 1.4.1 ships exactly three binaries worth hooking. This file is the 1.4.1 counterpart of [hooks.md](hooks.md), which covers the final 1.4.6 build of 2022-12-23.

The point of mapping it is that a user on 1.4.1 should not have to install 1.4.6 beside it to run FM8.plus, and a user who needs the 32-bit plugin has no later build to move to.

## 1. Binary identity

| Binary | Arch | Image base | PE TimeDateStamp | Functions |
|---|---|---|---|---|
| `FM8_141_EXE/FM8.exe` | x64 | `0x140000000` | `0x56266040` | 50,772 |
| `FM8_141_VST_64/FM8.dll` | x64 | `0x180000000` | `0x56266059` | 49,782 |
| `FM8_141_VST_32/FM8.dll` | **x86** | `0x10000000` | `0x56265f2b` | 48,027 |

The three timestamps differ, one per binary. In 1.4.6 all three share `0x63a57e00`, which is why [rvas.h](../src/core/rvas.h) can gate on a single `kFm8TimeDateStamp`. Supporting 1.4.1 needs three constants, matched per host.

1.4.1 carries roughly 8,000 more functions per binary than 1.4.6. It is a different codebase, not a rebuild.

Ghidra projects live beside the others in `FM8_DISASM` as `FM8_141_{EXE,VST_64,VST_32}_GHIDRA_{PROJ,ANALYSIS}`. Query them with the keys `exe141`, `vst64_141` and `vst32_141`.

```bash
python tools/q.py vst64_141 fn 0x18008c180
```

## 2. How the mapping was made

Text fingerprints alone cannot separate FM8's small helper functions, since hundreds of them share the same few tokens. [tools/bindiff141.py](../tools/bindiff141.py) instead seeds on evidence that is unique on both sides, a normalized function body or a distinctive set of string literals, then grows the match along the call graph, where a candidate pair is judged by how many of its callees and callers are already matched to each other. That relation survives a seven-year rebuild even where the code around it moved.

Two independent routes reach the 1.4.1 EXE and the 32-bit plugin. The direct route matches 1.4.6 to 1.4.1 across seven years. The composed route goes 1.4.6 to the 1.4.1 x64 plugin, verified by hand, then sideways within 1.4.1, where both binaries came out of the same compiler on the same day. [tools/compose141.py](../tools/compose141.py) walks the composed route and flags any site where the direct route disagrees. Eleven EXE sites were reached both ways and **no site disagreed**.

Addresses in the table below are one of three grades. Sites in `tools/anchors/*.txt` were read side by side in the decompiler and each carry a one-line note saying what confirmed them. The four EXE MIDI-out entries were found by API name, each occurring in exactly one function on both sides. The rest come from the matcher and are labelled below.

## 3. Hook table

RVAs, so add the runtime module base. Subtract nothing: these are already image-base relative.

| Site | `exe141` | `vst64_141` | `vst32_141` |
|---|---|---|---|
| kEngineStepGen | `0x0e7c70` | `0x0dc2b0` | `0x0aa5b0` |
| kEngineEmitNoteOn | `0x0e78a0` | `0x0dbee0` | - |
| kMidiArrayPush | `0x0e6c50` | `0x0db290` | - |
| kArpRunDispatch | `0x0a1570` | `0x094370` | `0x0639c0` |
| kMidiEventHandler | `0x0a08f0` | `0x0936f0` | `0x062d60` |
| kArpEngineWrapper | `0x0e7c60` | `0x0dc2a0` | `0x0aa5a0` |
| kNoteEventRouter | `0x088e40` | `0x07b990` | `0x04ee40` |
| kPerBlockProcessor | `0x0ffd90` | `0x0f4210` | `0x0c14d0` |
| kArpNoteFilter | `0x0e9ba0` | `0x0de160` | `0x0abee0` |
| kArpSyncRunState | `0x0ebbc0` | `0x0e0060` | - |
| kArpParamSetter | `0x0e5ee0` | `0x0da520` | `0x0a8cb0` |
| kVstSetParameter | `0x0ac050` | `0x0a0170` | `0x06eca0` |
| kParamDescLookup | `0x0936c0` | `0x086210` | `0x057010` |
| kParamDescBuilder | - | `0x090940` | - |
| kEngineSetDispatch | `0x099c70` | `0x08c7c0` | `0x05da80` |
| kSetParameterByTag | `0x099630` | `0x08c180` | `0x05d2b0` |
| kRawValueStore | `0x0997d0` | `0x08c320` | - |
| kCcEmitter | `0x099160` | `0x08bcb0` | `0x05ce50` |
| kVst2SendVstEvents | - | `0x102890` | - |
| kVst2MidiEventBuild | - | `0x056e70` | - |
| kVstPluginMain | - | `0x053a80` | `0x02b120` |
| kNICreatePlugin | - | `0x0539a0` | `0x02b040` |
| kAudioEffectXCtor | - | `0x1009f0` | - |
| kAEffectDispatcher | - | `0x1012c0` | - |
| kExePortSend | `0x2f9540` | - | - |
| kExePortOpen | `0x2f93b0` | - | - |
| kExeStreamSend | `0x2f95e0` | - | - |
| kExeDriverEnumerate | `0x2f9e70` | - | - |
| kUiaAppObject | `0x47e510` | `0x45a290` | `0x362f40` |
| kMenuBuilder | `0x0cf330` | `0x0c3810` | `0x093cf0` |
| kMenuCommandSink | `0x0c4020` | `0x0b8400` | `0x08a6a0` |
| kPopupAddItem | `0x469a80` | `0x445b40` | `0x3530f0` |
| kPopupAddSeparator | `0x46a090` | `0x4460c0` | `0x353510` |
| kPopupSetCheckState | `0x4761d0` | `0x452180` | - |
| kShowAboutDialog | `0x0bdcf0` | `0x0b20f0` | `0x084ef0` |

Both plugin entry points come from the export table, not the matcher. `VSTPluginMain` and `main` share one address in both plugins, and `NICreatePlugInInstance` sits just below it. The 32-bit `VSTPluginMain` passes uid `0x4e696638`.

`kProcessReplacing` is absent from the table for the same reason it is `0` in 1.4.6, it is a bare code label rather than a function. Capture it from `AEffect+0x78` at runtime. The 1.4.1 x64 `kAudioEffectXCtor` at `0x1009f0` shows the AEffect embedded at `this+0x30`, with the dispatcher written to `AEffect+0x08`, `setParameter` to `+0x18` and `getParameter` to `+0x20`.

## 4. What moved between 1.4.6 and 1.4.1

**FM8EditBuffer is unchanged.** `kSetParameterByTag` is the same function on both sides down to the constants, the `0x6b` clamp to 10..2000, the parameter array at `+0x68 + tag*4`, the dirty bits at `+0x29f0 + tag`, `+0x3955`, `+0x3957`, `+0x2a05`, `+0x2a0d`, `+0x395a` and `+0x395b`, and the morph branch on `tag - 0x84 < 2`. Everything the Morph CC feature touches carries over as is.

**The arp Engine did move.** Its fields sit about `0x5000` higher in 1.4.1, and not by a constant, so each field needs its own mapping.

| Field | 1.4.6 | 1.4.1 x64 |
|---|---|---|
| `kEngMidiArray` | `0x4840` | `0x9840` |
| `kEngOutCount` | `0x4848` | `0x9848` |
| `kEngPlaymode` | `0x4878` | `0x9870` |
| `kEngClock` | `0x4928` | `0x99c0` |

The clock's run gate is still the byte at `clock+0x14`, and `FM8EditBuffer -> Arp` is still `+0x29e8` with `Arp -> Engine` still `+0x20`.

**The 32-bit plugin is `__thiscall` throughout** and its MidiEventArray is packed for 4-byte pointers. Count moves from `+0x08` to `+0x04`, data from `+0x10` to `+0x08`, and the element stride from `0x28` to `0x20`. Its EditBuffer dirty bits land at `+0x2998`, `+0x29ad`, `+0x29b5`, `+0x3902` and `+0x3903`. Every detour for this binary needs its own signature and its own offsets, not a recompile of the x64 ones.

**`kPerBlockProcessor` lost an argument.** 1.4.6 takes a trailing `char headless`, 1.4.1 does not.

**`kRawValueStore` folds in the thread flag.** 1.4.6 stores with three arguments and does the thread dispatch in the caller, 1.4.1 takes a fourth argument and does it itself.

## 5. What ports, and what does not

**Morph CC ports cleanly** to all three 1.4.1 binaries. The setter, the CC emitter, the MIDI event handler and the parameter descriptor path are all mapped, and the EditBuffer layout is identical.

**Arp MIDI out ports to all three.** The engine chain, the note filter, the run state and the EXE's four `midiOut*` entry points are all mapped, and the 32-bit plug-in drives the arpeggiator through the same detours.

**The "+" wordmark ports as is.** 1.4.1 and 1.4.6 share 432 of 462 GUI resources byte for byte, including `PICTURE 193`, the wordmark the "+" is drawn into. Of 328 PICTUREs only `10007.png` differs. The differences are fonts, 16 FNT entries, and 12 forms, and 1.4.1 has one form 1.4.6 dropped, `FRM 14`.

**GUI Scale needed reimplementing, and now works.** FM8.plus scales 1.4.1 at the Win32 boundary instead, intercepting the calls NI::UIA's own layer would have adjusted (`CreateWindowExW`, `SetWindowPos`, `GetClientRect`, `GetWindowRect`, `InvalidateRect`, `BeginPaint`, `ScreenToClient`, `ClientToScreen`, `SetDIBitsToDevice`), plus `DialogBoxIndirectParamW`, whose frame USER32 creates without going through FM8's `CreateWindowExW`, so it is grown after `WM_INITDIALOG`. Top-level sizes scale only the client area and keep the frame. Every window FM8 creates gets a mouse subclass at birth that divides coordinates by the scale, which covers dialog contents as well as the main window. The standalone owns every window in the process, so the create hook registers nothing: registering a child used to switch the standalone over to the hosted window list, leaving its main window at 1x. One implementation covers x64 and x86 and needs no 1.4.1 addresses at all. The original finding stands and is why:

**FM8's own HiDPI layer cannot be ported.** FM8's HiDPI path is NI::UIA's own, and it does not exist in 2015. `GetDpiForWindow` appears in no 1.4.1 binary, and neither does `StretchDIBits`, so there is no scaled blit to switch on and no gate byte to flip. The feature would have to be built from nothing rather than woken up, which is a different project from the one [gui-runtime.md](gui-runtime.md) describes. `kUiaAppObject` is mapped anyway since it locates the app object for other uses.

**The menu has no submenus in 1.4.1,** which turns out not to matter. The 1.4.1 menu builder calls 7 helpers against 1.4.6's 14 and the popup class has no `SetSubmenu` taking `(menu, index, submenu)`, but FM8.plus builds its feature menu with its own Win32 `AppendMenuW` calls rather than FM8's popup class, so its nested items are unaffected.

## 6. Runtime layout differences

These are dereferenced by the detours, so they are per-build in `rvas.h`'s `Layout` table rather than shared constants.

| | 1.4.6 | 1.4.1 x64 | 1.4.1 x86 |
|---|---|---|---|
| `DspCore -> FM8VstObject` | `+0x08` | `+0x08` | `+0x04` |
| `FM8VstObject -> FM8EditBuffer` | `+0x55d0` | `+0x55a0` | `+0x5454` |
| `FM8EditBuffer -> Arp` | `+0x29e8` | `+0x29e8` | `+0x2994` |
| MidiEvent word order at `+0x10` | `[data2][data1][status]` | `[status][data1][data2]` | as x64 |
| MidiEventArray count / data / stride | `+0x08` / `+0x10` / `0x28` | same | `+0x04` / `+0x08` / `0x20` |
| `FM8VstObject -> FM8App` | `+0x5620` | vtable walk | vtable walk |

The byte order is the dangerous one, since both builds put the word at `+0x10` and nothing crashes if it is read the wrong way round, the MIDI just comes out scrambled. Each build's own handler settles it: it masks the data bytes with `0x7f` and switches on `(status & 0xf0)`.

The About argument is not a fixed field in 1.4.1. Its menu sink reaches the app object through `vtbl[0x128]` then `vtbl[0x8]`, so `Layout::vstObjApp` is 0 there and the About FM8 item stays greyed until that path is traced.

The 32-bit plug-in is `__thiscall` throughout, so its detours take `this` in ECX with the rest on the stack. MSVC will not put `__thiscall` on a free function, so FM8.plus declares `__fastcall` with an unused EDX slot, which lays the arguments out identically and cleans the same number of bytes. The disassembly settles the counts: the arp dispatch ends `RET 0x8` for its two stack arguments and the morph setter `RET 0xc` for its three.

## 7. Open items

- The 32-bit VST2 host adapter. `kVst2SendVstEvents`, `kVst2MidiEventBuild`, `kAudioEffectXCtor` and `kAEffectDispatcher` are unmapped for `vst32_141`. This is the layer that differs most between x86 and x64, so the matcher gets no traction and these need reading. Arp MIDI out for the 32-bit plugin is blocked on them.
- `kPopupSetCheckState` for `vst32_141`.
- The 1.4.1 App-object vtable walk, which gates the About FM8 menu item on both 1.4.1 builds.
- `kArpDataFieldWriter` and `kTimeSortedQueueIt` are unmapped everywhere. The first is reachable from `kArpParamSetter`'s callees. The second is a refutation entry in hooks.md rather than a hook, so it can stay unmapped.
- `kDispatcherWrapper`. Three candidates sit next to `kNICreatePlugin` in the x64 plugin, at `0x053910`, `0x053860` and `0x0538d0`.

## 8. Reproducing this

```bash
python tools/build_fm8_ghidra.py --only exe141,vst64_141,vst32_141
python tools/bindiff141.py vst2 vst64_141 --anchors tools/anchors/vst2-vst64_141.txt --save map.tsv
python tools/compose141.py --map vst2:vst64_141=map.tsv --anchors vst2:vst64_141=tools/anchors/vst2-vst64_141.txt --map vst64_141:exe141=map2.tsv --to exe141 --check exe:exe141=map3.tsv
```

`--explain` on bindiff141 lists neighbour-constrained candidates for anything still unmatched, which is how the remaining sites in section 6 should be chased.
