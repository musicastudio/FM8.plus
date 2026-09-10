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

VST2: the standard canDo strings `sendVstEvents`, `sendVstMidiEvent`,
`sendVstMidiEventFlagIsRealtime`, `receiveVstEvents`, `receiveVstMidiEvent` exist at
0xb8a0c0. Which ones the dispatcher answers with 1 is a decomp question.

VST3: bus name strings `Midi In` and `Midi Out` and the class `Vst::EventBus` exist, so the
NI VST3 wrapper has code for an event output bus. Whether FM8 registers it is a decomp
question.

EXE: imports `midiOutOpen`, `midiOutShortMsg`, `midiStreamOut` and has
`NI::NSA::WinMidiOutputDevice`, `NI::SEQ::MidiOut`, preference strings `Pref Send MIDI
Channel` and `CC Send Midi Channel:`. The standalone already owns a MIDI output device
abstraction (used for controller feedback or MIDI clock) that arp MIDI out can reuse.
