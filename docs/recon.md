# Static recon (strings and PE headers)

All three binaries share PE TimeDateStamp `0x63a57e00` (2022-12-23), MSVC 2015+ toolchain
(VCRUNTIME140, MSVCP140), x64. Section layouts differ, so addresses are per binary.

| Binary | Image base | .text size | Exports |
|--------|-----------|-----------:|---------|
| FM8.exe | 0x140000000 | 0xa7360c | none (GUI app) |
| FM8.dll (VST2) | 0x180000000 | 0xa1849c | `VSTPluginMain`, `NICreatePlugInInstance` |
| FM8.vst3 | 0x180000000 | 0xa2932c | `GetPluginFactory`, `InitDll`, `ExitDll` |
| FM8 FX.dll | 0x180000000 | 0x4990 | `VSTPluginMain` (46 KB shim, LoadLibrary + GetProcAddress) |

Strings dumps: `FM8_DISASM/strings/strings_{exe,vst2,vst3}.txt` (file offset, text).

## NI framework (from MSVC RTTI names)

Namespaces by frequency: `NI::K2IMPORT` (Kore 2 import layer, 2274 classes), `NI::NGL`
(GUI), `NI::SOUND`, `NI::SC3`, `NI::SND`, `NI::UIA`, `NI::GP`, `NI::AB` (audio bridge /
plugin interface), `NI::PA`, `NI::MEDIA`, `NI::SEQ`, `NI::VST2`, `NI::FM8DSP`.

FM8-specific classes (global namespace): `FM8PlugIn`, `FM8Midi`, `FM8MIDIArpeggiator`,
`FM8ArpeggiatorMacro`, `Arpeggiator`, `ArpeggiatorForm`, `FormArp`, `MorphForm`, `MorphLink`,
`MorphSelector`, `XYHandleMorph`, `MainModule`.

Arpeggiator engine (NI namespace): `MIDIArpeggiator`, `MIDIArpeggiator_Engine`,
`MIDIArpeggiator_Clock`, `MIDIArpeggiator_DataContainer`,
`MIDIArpeggiator_SafeSwapDataContainer`, list items `StepPositionLI`, `NoteOffPositionLI`,
`VelocityLI` in `TimeSortedSequence<>`. The Kore module wrapper is
`NI::K2IMPORT::DESC::MIDI_ArpeggiatorModule` with enum parameters `ePlaymode`, `eDenominator`,
`eDenominatorExt`, `eForceKeysMode`, `eScalemode`, `eRootkey`, `eAutochord`,
`ePlayQuantisationWithStep`.

MIDI plumbing in the Kore layer: `MidiInputPin`, `MidiOutputPin`, `MidiInputBus`,
`MidiOutputBus` (`NI::K2IMPORT::EAL`), `WireImpl<MidiInputPin, MidiOutputPin>`,
`MidiEventArray` (NI), `BeatTickTimeEventList<MidiEvent>` (NI::AB). The arpeggiator is a
MIDI module with a MIDI output pin wired to the sound module's MIDI input, which is the
natural tap point for arp MIDI out.

Host interfaces: `NI::AB::InterfaceVST`, `InterfaceVSTHeadless`, `NI::VST2::App<...FM8,
FM8PlugIn>`, `NI::VST2::EditorControllerUIA`, `InterfaceVST3`, `FM8VST3PlugIn`.

## Feature 1: mod wheel to morph

Parameter names (long / short triplets in .rdata, VST2 file offset 0xa262a0):
`Morph X`, `Morph Y`, `Morph Random X`, `Morph Random Y`, `Morph Random Seed`. Also
`Sound Variation Morph X/Y`, `Enable Morphing`, and Kore variables `MorphX`, `MorphY`,
`MorphByAutomation` (`NI::K2IMPORT::DESC::DETAIL::Morph*VariableProperties`).

Mod wheel shows up as a modulation source (`%s Modulation Modwheel`, `Controller 1`,
`Controller 2`, `TalkWah: ModWheel`). No existing mapping from mod wheel to morph.

## Feature 2: arpeggiator MIDI out

Arp parameter triplets at VST2 offset 0xaa4a20: On, Number of Steps, Note Length, Tempo,
Tripplets, Dotted, Fixed Velocity, Velocity, Accent, Mode, Down, Split On, Split Note,
Split Learn, Split Bass, Shuffle, Hold, Rotate Left/Right, Key Sync, BPM Sync, BPM, 1 Shot.
Note the developer string "Arpeggiator - KEY SYNC currently only works correctly with
external MIDI-Keyboard Input."

VST2 (verified headless with `tools/vst2host.py` against the installed FM8.dll): uniqueID
`0x4e696638` ('Nif8'), 1094 parameters, 128 programs, 0 in / 2 out, flags 0x139. canDo answers:
`sendVstEvents` 1, `sendVstMidiEvent` 1, `sendVstTimeInfo` 1, `receiveVstEvents` 1,
`receiveVstMidiEvent` 1, `receiveVstTimeInfo` 1, `offline` -1, `bypass` 0,
`sendVstMidiEventFlagIsRealtime` 0. On the first processed block the stock plugin already sends
two MIDI events to the host through `audioMasterProcessEvents`: CC7 (volume 102) and CC10
(pan 64). So the NI VST2 layer has a working, sample-stamped MIDI output path; arp MIDI out
only has to feed it. Parameter indices: `Morph X` 21, `Morph Y` 22, `Morph Random X` 23,
`Morph Random Y` 24, `Morph Rnd. Seed` 25, `Arpeggiator On` 136. Full list in
`docs/vst2_params.txt`.

VST3 (verified headless with `tools/vst3host.py`): two classes, `FM8` and `FM8 FX`, both
"Audio Module Class"; single-component (the component also implements `IEditController`).
Buses: audio out `Out 1` (stereo), event in `Event Input` (16 channels), **no event output
bus**. So the VST3 arp MIDI out needs a wrapper that adds an event output bus and fills
`ProcessData::outputEvents`. 1103 parameters; parameter ids equal the VST2 indices (`Morph X`
id 21, `Morph Y` id 22, `Arpeggiator On` id 136), plus a `Default` program parameter id
1295090176 and MIDI controller parameters at the end (`Controller 2`, `Hold Pedal`,
`Sustenuto Pedal`). `IMidiMapping` maps CC1 on channel 0 to parameter id 0x6d69646b, so in
VST3 the mod wheel reaches the plugin as a parameter change, not as a MIDI event. Full list in
`docs/vst3_params.txt`. The bus name strings `Midi In`/`Midi Out` and `Vst::EventBus` in the
binary belong to the generic NI VST3 layer, not to a registered FM8 bus.

Note: a process that has loaded FM8.vst3 or FM8.dll never fully exits (teardown blocks in
kernel), so the probe tools terminate themselves hard and may leave a zombie process behind.

EXE: imports `midiOutOpen`, `midiOutShortMsg`, `midiStreamOut` and has
`NI::NSA::WinMidiOutputDevice`, `NI::SEQ::MidiOut`, preference strings `Pref Send MIDI
Channel` and `CC Send Midi Channel:`. The standalone already owns a MIDI output device
abstraction (used for controller feedback or MIDI clock) that arp MIDI out can reuse.
