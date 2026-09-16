# FM8.plus Hook Reference

Reverse-engineered hook points for FM8 (Windows x64, build 2022-12-23). Image bases: VST2 `FM8.dll` `0x180000000`, VST3 `FM8.vst3` `0x180000000`, EXE `FM8.exe` `0x140000000`. All calls are Microsoft x64 (RCX, RDX, R8, R9; floats XMM0-3; `param_1` is usually `this`). Addresses are static; add the runtime ASLR delta from the image base. None of these are exported, so a byte-pattern or offset table against this exact build is required.

## 1. Hook table

### MIDI event core (shared engine, present in all three binaries)

| Target | Role | VST2 | EXE | VST3 | Signature | Conf |
|---|---|---|---|---|---|---|
| Engine step generator | Generates arp events for one position into MidiEventArray at engine+0x4840 | `0x1801370a0` | `0x140147740` | `0x180143bf0` | `MidiEventArray* f(Engine* this, int inBlockSamplePos)` | high |
| Note-ON emitter | Resolves note/velocity/mode, appends note-on + schedules note-off | `0x180136bc0` | `0x140147260` | `0x180143710` | `void f(Engine* this, uint voiceIndex, StepPositionLI* step, MidiEventTemplate* evt)` | high |
| MidiEventArray append | Single choke point writing packed word into array element+0x10 | `0x180135510` | `0x140145bb0` | `0x180142060` | `void f(MidiEventArray* arr, MidiEventTemplate* evt)` | high |
| Arp run + dispatch | Runs engine, drains array, forwards each event to synth voices | `0x1800e7250` | `0x1400fd9c0` | `0x1800f4310` | `void f(FM8DspCore* core, u32 destSel=2, int inBlockPos)` | high |
| MIDI event handler | THE convergence handler: status switch, note/CC/voice, CC1 mod-wheel | `0x1800e6660` | `0x1400fcdd0` | `0x1800f3720` | `void f(FM8Midi* this, MidiEvent* ev, int flag=2)` | high |
| Arp engine wrapper | Sets MXCSR, calls step generator; position passes through RDX | `0x180137050` | `0x1401476f0` | `0x180143ba0` | `MidiEventArray* f(ArpHolder* holder, int inBlockPos)` | high |
| Note-event router | Offers event to arp first; if not consumed, to voice allocator | `0x1800cc370` | `0x1400e3050` | `0x1800d9580` | `void f(FM8* core, MidiEvent* ev, u32, u32)` | high |
| Per-block processor | ~20KB render; drains time-sorted queue, routes type-1 events, feeds arp | `0x18014ee40` | `0x14015f4e0` | `0x18015b890` | `void f(SoundModule* this, float* in..., int nFrames, char headless)` | high |
| Time-sorted queue iter | Returns next queued event matching current sample index | `0x1806ce420` | `0x140842fc0` | `0x1806de5b0` | `char f(seqObj, u64* cursor, int key, u64* outEvent, int* outKey)` | high |

### Arp routing / state

| Target | Role | VST2 | EXE | VST3 | Signature | Conf |
|---|---|---|---|---|---|---|
| Arp note filter | Returns 1 if arp consumes note, 0 if passed to synth; Split + Key-Sync | `0x180138c30` | `0x1401492d0` | `0x180145690` | `char f(FM8MIDIArpeggiator* this, MidiEvent* ev, int off)` | high |
| Sync run state | Compares On flag to Clock+0x14, starts/stops transport | `0x18013b1e0` | `0x14014b880` | `0x180147c30` | `void f(Engine* this)` | high |
| Arp param setter | FM8EditBuffer apply route 2; KeySync/1Shot/Rotate + data writer | `0x180134850` | `0x140144ef0` | `0x1801413a0` | `bool f(FM8MIDIArpeggiator* this, int idx, float v, int mode)` | high |
| Arp data field writer | DataContainer field writer; case 0 writes On at data+0 | `0x180134980` | `0x140145020` | `0x1801414d0` | `bool f(char* data, int idx, float v, char flag)` | high |

### Morph setter (Morph X = tag 0x84, Morph Y = tag 0x85)

| Target | Role | VST2 | EXE | VST3 | Signature | Conf |
|---|---|---|---|---|---|---|
| VST setParameter | Entry for host/automation; denormalizes, forwards to engine dispatcher | `0x1800f98c0` | `0x14010a860` | `0x1801065e0` | `void f(FM8VstObject* this, uint index, float norm, uint thread)` | high |
| Param descriptor lookup | VST index to 5-dword descriptor record | `0x1800d7e20` | `0x1400eeb00` | `0x1800e5030` | `u32* f(int index)` | high |
| Descriptor builder | Maps index 21..25 to tags 0x84..0x88, kind 0 | `0x1800e3dd0` | `0x1400fa540` | `0x1800f0e90` | `bool f(int index, u64* outRec)` | high |
| Engine SET dispatcher | Switches on descriptor kind; kind 0 calls EditBuffer vtable+0x10 | `0x1800df7d0` | `0x1400f6330` | `0x1800ec9e0` | `intptr f(FM8EditBuffer* eng, u32 valueBits, u32* desc, uint thread, char flag)` | high |
| setParameterByTag | THE internal morph setter; stores value, recomputes quadrant, broadcasts | `0x1800dee10` | `0x1400f5970` | `0x1800ec020` | `intptr f(FM8EditBuffer* this, uint tag, float value, char thread)` | high |
| Raw value store | Writes float into EditBuffer+0x68+tag*4 | `0x1800e9b30` | `0x140100100` | `0x1800f6d50` | `void f(FM8EditBuffer* this, int tag, float valueBits)` | high |

### VST2 MIDI-out (VST2 only, absent from EXE and VST3)

| Target | Role | VST2 | EXE | VST3 | Signature | Conf |
|---|---|---|---|---|---|---|
| Send VstEvents to host | audioMaster opcode 8; only host-callback op-8 site | `0x1802ea500` | n/a | n/a | `bool f(HostAdapter* this, VstEvents* ev)` | high |
| VstMidiEvent builder | Builds one VstMidiEvent, wraps as VstEvents, calls op-8 sender (vtable slot 5 / +0x28) | `0x1802e79f0` | n/a | n/a | `void f(InterfaceVST* this, MidiRecord* ev)` | high |
| CC emitter | FM8Midi controller-out; builds packed record, pushes via vtable+0x28 (CC7/CC10 path) | `0x1800de810` | `0x1400f5370` | `0x1800eba20` | `void f(FM8Midi* this, uint cc, uint value, char hiRes)` | high |

EXE and VST3 have no VST2 audioMaster path; the CC emitter itself exists in all three but in EXE/VST3 its vtable-slot-0x28 target is a different (non-audioMaster) sink.

### VST2 process / instantiation (VST2 only)

| Target | Role | VST2 | Signature | Conf |
|---|---|---|---|---|
| VSTPluginMain | Exported entry; tail-calls NICreatePlugInInstance with uniqueID 0x4e696638 | `0x1800bd090` | `AEffect* f(audioMasterCallback host)` | high |
| NICreatePlugInInstance | Builds plugin via IApplication selector 0x1adb1; patches AEffect+8 (dispatcher) | `0x1800bcf20` | `AEffect* f(host, int uid, int, int=2)` | high |
| Dispatcher close-wrapper | Forwards to saved dispatcher; opcode 1 decrements refcount | `0x1800bce70` | `VstIntPtr f(AEffect*, int opcode, ...)` | high |
| AudioEffectX ctor | Fills AEffect (magic, dispatcher, processReplacing, flags 0x139, numParams 1094) | `0x1802e8660` | `AudioEffectX* f(this, host, int numPrograms, int numParams)` | high |
| processReplacing trampoline | AEffect+0x78 target; loads object+0x60, tail-calls C++ virtual | `0x1802ea480` | `void f(AEffect*, float** in, float** out, int frames)` | high |
| AEffect dispatcher | Generic AudioEffectX dispatcher at AEffect+8 (before wrap) | `0x1802e8f60` | `VstIntPtr f(AEffect*, int op, int idx, VstIntPtr val, void* ptr, float opt)` | high |

### VST3 buses (VST3 only)

| Target | Role | VST3 | Signature | Conf |
|---|---|---|---|---|
| createInstance | Allocates 0xc2dc8 FM8VST3PlugIn, binds 15 sub-object vtables | `0x1800bc830` | `tresult f(factory, int cidTag, FUnknown** args, void* ctx)` | high |
| IAudioProcessor::process | Consumes inputEvents, renders, flushes outbound Event ring to outputEvents | `0x1800c7920` | `void f(IAudioProcessor* this, ProcessData* data)` | high |
| IComponent::getBusCount | Returns BusList size; event-out count is data-driven, no gating flag | `0x18016da80` | `int32 f(this, MediaType, BusDirection)` | high |
| Event-out bus registrar | Pushes EventBus into event-out BusList (base+0x190) | `0x18016d4b0` | `f(componentBase, wchar* name, int channelCount, int)` | medium |

VST3 static vtable pointers (.rdata): FM8VST3PlugIn primary `0x180a36800`; IComponent `0x180a36ae8`; IAudioProcessor `0x180a36b60` (process slot at `0x180a36ba8`); IMidiMapping `0x180a36d48`.

### EXE hardware MIDI out (standalone only)

| Target | Role | EXE | Signature | Conf |
|---|---|---|---|---|
| Port send | WinMidiPortOutputDevice::send; only midiOutShortMsg site (vtable slot 10) | `0x140196f60` | `bool f(WinMidiPortOutputDevice* this, MidiEventArray* list)` | high |
| Port open | midiOutOpen with device id at this+0xc4 (vtable slot 3) | `0x140195730` | `bool f(WinMidiPortOutputDevice* this)` | high |
| Stream send | WinMidiStreamOutputDevice::send; timestamped midiStreamOut | `0x140197030` | `void f(WinMidiStreamOutputDevice* this, MidiEventArray* list)` | high |
| SysEx send | Builds 0xF0.. long message (vtable slot 11) | `0x1401972b0` | `u64 f(WinMidiPortOutputDevice* this, void** sysex)` | medium |
| Device base ctor | Records WinMM device id + name | `0x140188170` | `void f(this, void* parent, uint deviceId, std::string* name, uint)` | high |
| Driver enumerate | Builds MIDI in/out device lists for Audio & MIDI Settings | `0x14019ba70` | `void f(WinMidiDriver* this)` | high |
| CC feedback emitter | Stamps CC on Pref Send MIDI Channel, posts to engine out sink | `0x1400f5370` | `void f(FM8Midi* this, uint cc, uint value, char hiRes)` | high |

### UI menu (FormMain; present in all three)

| Target | Role | VST2 | EXE | VST3 | Signature | Conf |
|---|---|---|---|---|---|---|
| Menu builder | Populates File/Options popup + submenus | `0x18011dd00` | `0x14012e310` | `0x18012a890` | `void f(FormMain* this)` | high |
| Command sink | onControlNotify; controlId 0x18 dispatches menu items | `0x180113240` | `0x140123960` | `0x18011fdd0` | `void f(FormMain+0x248* this, int ctrlId, int notify, EventData* ev)` | high |
| PopupMenu::addItem | Appends item, sets labels + command id at item+0x60; returns index | `0x18070b4d0` | `0x140753f10` | `0x18071b920` | `int f(PopupMenu* this, char* long, char* short, int cmdId)` | high |
| PopupMenu::addSeparator | Appends separator (item+0x6c=1) | `0x18070b9d0` | `0x140754410` | `0x18071be20` | `void f(PopupMenu* this)` | high |
| PopupMenu::setItemSubmenu | Attaches child menu (item+0x78, submenu+0xd8=owner) | `0x180717380` | `0x14075fe50` | `0x1807277d0` | `bool f(PopupMenu* this, int idx, PopupMenu* submenu)` | high |
| PopupMenu::setItemCheckState | Writes item+0x70 (1=checked, 2=unchecked box) | `0x180717260` | `0x14075fd30` | `0x1807276b0` | `bool f(PopupMenu* this, int idx, int state)` | high |

### GUI scale (NI::UIA window layer; present in all three)

| Target | Role | VST2 | EXE | VST3 | Signature | Conf |
|---|---|---|---|---|---|---|
| UIA app object | Returns the object whose byte +0x49 gates the whole HiDPI path | `0x180731140` | `0x140779cc0` | `0x180741530` | `void* f(void)` | high |
| Window DPI scale | `GetDpiForWindow(hwnd)/96`, via Shcore | `0x180737b00` | `0x140780500` | `0x180747ef0` | `float f(HWND)` | high |
| Window surface scale | `ceil(dpi scale)`: the integer factor the DIB is rendered at | `0x180738aa0` | `0x140781630` | `0x180748e90` | `float f(Window*)` | high |

## 2. Per-target notes

### Arp emit and the event path

Per block, DSP core `FUN_1800f84f0` preps the engine (`FUN_180138970`) then renders `FUN_18014ee40`, which walks the block in sub-blocks bounded by scheduled events and, at each boundary, calls the arp run+dispatch `0x1800e7250`. That calls the MXCSR wrapper `0x180137050` then the step generator `0x1801370a0`, which clears the output count (engine+0x4848=0), advances the step/note-off sequences, writes note-OFF events inline (status 0x80), and delegates note-ON to `0x180136bc0`. Generated events land in the MidiEventArray at engine+0x4840; back in `0x1800e7250` each element is passed to the handler `0x1800e6660` to trigger voices.

Recommended detour for observation: `0x1800e7250`. Call the original, then read the returned array: count `*(int*)(arr+0x08)`, data `*(void**)(arr+0x10)`, stride 0x28; per element decode the packed word at elem+0x10 (status=(w>>16)&0xff, data1=(w>>8)&0x7f, data2=w&0x7f). The in-block sample position is the 3rd argument (R8). For MIDI-only mode (engine runs, synth silent), wrap the handler `0x1800e6660` to record then return early, or skip the dispatch loop in `0x1800e7250`. Do not touch `0x180137050`/`0x1801370a0`, since step advance, note-off scheduling and the clock must keep running. Do not pre-zero the array count; the generator resets it itself.

**MidiEventArray** (engine+0x4840): +0x00 vftable; +0x08 uint count; +0x10 void* data; +0x18 void* end; capacity = (end-data)/0x28. Element stride 0x28; only the packed word at elem+0x10 is populated. No per-event timestamp, as timing is the sample position of the dispatch call. Evidence: `0x180135510` (count@+8, end@+0x18, stride /0x28), consumer `0x1800e7250` (`lVar2 += 0x28`).

**MidiEventArrayElem** (0x28): elem+0x10 byte0 = data2/velocity, byte1 = data1/note, byte2 = status, byte3 = internal flags (0x20 channel, 0x10 when status>=0xf0). Evidence: writer `0x180135510`, reader `0x1800e6660`.

**Engine selected offsets**: +0x4838 chord/voice count; +0x4840 MidiEventArray; +0x4848 out count; +0x4868 SplitOn, +0x486c SplitNote, +0x4870 SplitDir, +0x4871 Hold; +0x4878 playmode switch; +0x4890/+0x4898 note-off list; +0x4928 primary Clock; +0x4a30 HostTimeInfo; +0x4ae0/+0x4ae4/+0x4ae8 scheduled sample positions; +0x4af0 secondary Clock; +0x4af8 offline; +0x4b00 transport; +0x4b08 PRNG. 240000 ticks per quarter. Evidence: `0x1801370a0`, `0x180136bc0`, ctor `0x180131cf0`.

**StepPositionLI** (param_3 of `0x180136bc0`): +0x00 beat, +0x04 tick; +0x18/+0x1C note-off delta beat/tick (0,0 = same-block off); +0x38 (param_3[0xe]) velocity; +0x40 (param_3[0x10]) base note; +0x44 legato/skip flag; +0x3c (param_3[0xf]) chord selector (0xFFFFFFFE = all voices, 0xFFFFFFFF = random). Evidence quoted in `0x180136bc0`/`0x1801370a0`. Confidence high.

### Arp routing and the On gate

Graph: FM8 core+0x55d0 -> FM8EditBuffer; editbuf+0x29e8 -> FM8MIDIArpeggiator; arp+0x20 -> Engine; engine+0x4928 -> Clock. Live "arp running" gate is the byte at `*(Engine+0x4928)+0x14`. Persisted On param is `data[0]` where `data = *(*(arp+0x10)+0x3b8)`.

Per event, `0x18014ee40` -> router `0x1800cc370` -> filter `0x180138c30`. If Clock+0x14==0 the filter returns 0 and the router forwards to the voice allocator `0x1800e6660`. If running, Split lets pass-through notes return 0; otherwise returns 1 (consumed).

To force arp on/off programmatically: set `data[0]` then call sync `0x18013b1e0(*(arp+0x20))`. Best single diversion hook: detour the filter `0x180138c30` (return 0 to disable diversion without touching the On param).

**FM8MIDIArpeggiator** (0x50): +0x08 keySyncLatch; +0x10 DataContainer*; +0x18 SafeSwap*; +0x20 Engine*; +0x28 hostCtx (VstTimeInfo@+0x250); +0x30 owner; +0x40 seq. **DataContainer data** (via +0x3b8): +0x00 On, +0x04 Steps, +0x08 NoteLen, +0x0c denom, +0x10 tripletMode, +0x18 Velocity, +0x1c Accent, +0x2c SplitNote, +0x34 Shuffle, +0x3e BPM-Sync, +0x40 BPM. Evidence: ctor `0x1801318f0`, `0x180134980`.

### Morph setter data path

Host writes normalized value to VST index 21 (Morph X) or 22 (Morph Y): `0x1800f98c0(plugin, index, norm, thread)`. Descriptor lookup `0x1800d7e20` yields {kind=0, tag=0x84/0x85}. Value denormalized to min+(max-min)*norm (morph range 0..1). Engine dispatcher `0x1800df7d0(engine=*(plugin+0x55d0), scaled, desc, thread)` dispatches kind 0 to EditBuffer vtable+0x10 = `0x1800dee10`. That stores the float at engine+0x68+tag*4 (MorphX=+0x278, MorphY=+0x27c) via `0x1800e9b30`, sets dirty flag engine+0x3955, and since tag-0x84<2 calls quadrant recompute `0x1800e2050` (reads +0x278/+0x27c, writes quadrant at +0x28b0), then broadcasts via `0x180143370`. GUI drag uses the same engine setter in reverse.

Recommended call (both sound and handle follow): get `eb = *(void**)(plugin+0x55d0)`, `vtbl = *(void***)eb`, `setByTag = vtbl[2]` (slot +0x10), then `setByTag(eb, 0x84, x, 1)` and `setByTag(eb, 0x85, y, 1)` with x,y in 0..1. Simpler: call `0x1800f98c0(plugin, 21/22, norm, thread)`. Do NOT raw-write +0x278/+0x27c; that skips the quadrant recompute and the broadcast.

**FM8EditBuffer** (at FM8VstObject+0x55d0, size 0x39e0, vtable 0x180a27d28): param array base +0x68 indexed by tag; MorphX +0x278, MorphY +0x27c, Rnd X/Y/Seed +0x280/+0x284/+0x288; +0x28b0 active quadrant; +0x3955 timbre dirty; +0x53c owner; +0x53d sub-param object. Vtable slots: +0x10 setParameterByTag, +0x58 getParameterByTag, +0xa8 value transform. Evidence: ctor `0x1800c0650`, setter `0x1800dee10`.

### CC handler and CC1 mod-wheel

The convergence handler `0x1800e6660` has no channel filter (FM8 is omni). Packed word at ev+0x10. The 0xB0 branch does MIDI-learn (map at this+0x734), assignable controllers (this+0x71d/+0x71e), then `switch(controller)`. CC1 (case 1) writes value as float to engine+0x3988, sets active flag engine+0x3945, and notifies mod source #2 via `0x1801473e0`. CC2 (case 2) writes engine+0x398c.

Recommended CC1-to-morph detour: hook `0x1800e6660` (VST2/EXE). Read `w = *(u32*)(ev+0x10)`; if `(w>>16)&0xF0 == 0xB0 && (w>>8)&0x7F == 1`, map `w&0x7F` to a morph angle and set Morph X/Y, then swallow or forward as desired. CC1 is hardwired to case 1, independent of user Controller1/Controller2 assignments.

VST3 caveat: the VST3 host remaps CC1/ch0 to parameter id `0x6d69646b` via IMidiMapping, so a raw CC1 event likely never reaches `0x1800f3720`. For a VST3 build, also intercept the parameter apply-path for id 0x6d69646b. This apply-site was not traced (see open questions).

**Note: two functions originally suspected in the CC path are refuted.** `FUN_18054bf30` (vst2) / `0x14059e790` (exe) / `0x18055c620` (vst3) is SQLite `sqlite3VdbeSerialGet`, not a MIDI byte packer; its "status<<16|data1<<8|data2" is a coincidental 24-bit big-endian serial-type decode. `0x1806ce420` is the time-sorted queue iterator, not the CC handler. Do not hook either for MIDI.

### VST2 MIDI-out

Emitters build a 0x28 internal record and push it through the host-adapter vtable slot 0x28 = `0x1802e79f0`, which formats a VstMidiEvent (type=1, byteSize=0x18, deltaFrames=min(record offset, blockSize@adapter+0xaa8)), wraps it in a 1-element VstEvents, and calls `0x1802ea500`, which invokes `audioMaster(adapter+0x30, opcode 8, 0, 0, VstEvents*, 0)`. Host already returns canDo=1 for sendVstMidiEvent.

Recommended: call the slot-5 method directly. `adapter = *(void**)(*(void**)(FM8Midi+8)+8)`. Build a 0x28 record: sample offset at rec+0x08, packed word at rec+0x10 ([data2][data1][status] little-endian). Then `(*(void(**)(void*,void*))(*(void**)adapter + 0x28))(adapter, &rec)`. Run it from inside the process cycle so sample offsets stay valid. EXE and VST3 have no audioMaster path; this target is missing there.

### VST2 process

Host calls AEffect.processReplacing (pointer `0x1802ea480`, AEffect+0x78), which reads AEffect.object (+0x60) and tail-calls the object's C++ processReplacing. The NI framework layer drives `0x1800f84f0` then the engine render `0x18014ee40`, which runs the MIDI drain synchronously in the same call. The arp engine uses musical time (beat ticks, 240000/quarter), not VST sample offsets. For arp MIDI-out, tap `0x180138970` (per-block arp entry) and read the array at engine+0x908 after it returns. These instantiation/AEffect functions are VST2-only (absent from EXE and VST3).

**AEffect** (at AudioEffectX+0x30): +0x00 magic 'VstP'; +0x08 dispatcher; +0x10 process; +0x18 setParameter; +0x20 getParameter; +0x2C numParams (1094); +0x38 flags (0x139); +0x60 object (self); +0x70 uniqueID (0x4e696638); +0x78 processReplacing; +0x80 processDoubleReplacing. Evidence: ctor `0x1802e8660`.

### VST3 buses

The generic VST3 bus machinery fully supports an event-output bus; it is missing only because FM8's device descriptor lists zero MIDI-out buses, so the event-out BusList (componentBase+0x190) stays empty and getBusCount(kEvent,kOutput) returns 0. There is no boolean flag to flip. Enablement: (1) register a bus by calling `0x18016d4b0(componentBase, u"Midi Out", 16, 0)` so getBusCount reports 1 and the host allocates data.outputEvents; (2) fill output events. process `0x1800c7920` already flushes an internal Event ring at procThis+0xc50 (count +0xc58) to `outputEvents->addEvent` (IEventList vtbl+0x28), but nothing stages into it. Either push 48-byte Vst::Event records into that ring and bump +0xc58 before the flush, or detour the process vtable slot at `0x180a36ba8` to write `data.outputEvents->addEvent` directly.

**ProcessData**: +0x08 numSamples, +0x18 inputs, +0x20 outputs, +0x28 inputParameterChanges, +0x30 outputParameterChanges, +0x38 inputEvents, +0x40 outputEvents. **IEventList vtable**: +0x18 getEventCount, +0x20 getEvent, +0x28 addEvent; Event size 0x30. Evidence: `0x1800c7920`.

This whole surface is VST3-only; there is no port to VST2 or EXE.

### EXE hardware MIDI out

FM8App owns a WinMidiDriver registered via `0x1401964d0`; `0x14019ba70` enumerates devices, storing the WinMM index at device+0xc4. On user selection, `0x140195730` opens the port (HMIDIOUT at device+0xf0, isOpen byte at +0xc0). To emit, pass a MidiEventArray (head at +8, nodes linked by +0x38, type 1 at node+0x04, packed word at node+0x10) to `0x140196f60`, which locks device+0x1f and calls `midiOutShortMsg`.

Recommended send: hook or call `0x140196f60` (RCX = device, RDX = MidiEventArray). Lowest-level fallback: read HMIDIOUT at device+0xf0 and call midiOutShortMsg yourself (device open when byte at +0xc0 is 1). Channel-aware path reusing existing plumbing: `0x1400f5370(FM8Midi=engineCore+0x55e0, cc, value, 0)`, gated by Dump-Ctrls byte at FM8Midi+0x1c51 (force to 1 if disabled). This entire WinMM subsystem is standalone-only; absent from VST2 and VST3 (both route MIDI through the host).

**FM8Midi** fields (shared across all three): +0x08 engine/owner; +0x518[128] velocity cache; +0x71d Controller1 (default 4); +0x71e Controller2 (default 0x0b); +0x734[128] CC-to-param learn map; +0x1c51 Dump-Ctrls gate; +0x1c64 send MIDI channel (1..16). Evidence: ctors `0x1800c0c80`/`0x1400d79e0`, handler `0x1800e6660`.

### UI menu

FormMain builds the File/Options popup in `0x18011dd00`; selections are serviced by the command sink `0x180113240` (this = FormMain+0x248). The File ButtonMenuControl is FormMain[0x59] = FormMain+0x2c8, its embedded PopupMenu at +0x1e0.

Inject items: detour `0x18011dd00`, call original, then on `filePopup = *(FormMain+0x2c8)+0x1e0`: addSeparator `0x18070b9d0`; new submenu `op_new(0x1d8)` + PopupMenu ctor `0x18070a6d0`; `0x18070b4d0(filePopup, "FM8.plus", "FM8.plus", 0x1000)` then `0x180717380(filePopup, idx, submenu)`; add toggle items with ids 0x1001/0x1002; set checks via `0x180717260(submenu, i, state?1:2)`. Service clicks: detour `0x180113240`; when controlId (EDX) == 0x18, read firing menu `*(*(R9+8)+0x18)` and index `*(int*)(*(R9+8)+0x10)`, read cmdId at `menu+0x158[idx]*8 + 0x60`, toggle, re-render, and return without calling original to swallow.

**MenuItem** (~0x80): +0x60 commandId (read by sink), +0x6c isSeparator, +0x70 checkState (0 plain, 1 checked, 2 unchecked box), +0x78 submenu. **PopupMenu** (0x1d8): +0x00 vftable (0x180cbf798), +0xd8 owner form, +0x158/+0x160/+0x168 item vector. Evidence: item ctor `0x18070b560`, builder `0x18011dd00`.

### The logo click and the About panel

Control 5 of FRM 5/15 is the "FM8" wordmark (a Switch, mode 0, so it notifies on mouse-down only). Its click reaches the same FormMain command sink, `case 5`, which does one thing:

```c
app = *(void**)(*(void**)(sink_this + 0x10) + 0x5620);   // FM8VstObject -> FM8App
ShowAboutDialog(app);                                     // builds FM8AboutDialog, runs it modally
```

| Target | Role | VST2 | EXE | VST3 | Signature | Conf |
|---|---|---|---|---|---|---|
| Show About dialog | ResourceManager::Init(8000,8000), constructs `FM8AboutDialog`, runs it at size {0x40,0x88} | `0x18011b4b0` | `0x14012bac0` | `0x180128040` | `void f(FM8App* app)` | high |

Getting `app` needs no new hook: `*(sink_this + 0x10)` and `*(ArpRunDispatch_core + 8)` are handed to the same two accessors (`*(x+0x55d0)` = FM8EditBuffer, `*(x+0x55e0)` = FM8Midi), so they are the same FM8VstObject, and FM8.plus already walks it every block for the morph EditBuffer. `*(FM8VstObject + 0x5620)` is the FM8App back-pointer, written once at construction by `0x140130fd0(app, vstObj)`. So the About panel FM8.plus offers is FM8's, called the way FM8 calls it, with the pointer of the instance that was clicked.

Dialogs are modal and run their own message loop on the UI thread, so the call only returns when the user closes the panel.

### GUI scale

NI::UIA carries a complete HiDPI layer that FM8 never switches on. `Window::create` (`0x140780ad0`)
multiplies the requested size by the window's DPI scale before `CreateWindowExW`; `Window::setSize`
(`0x1407844f0`) does the same before `SetWindowPos`; `WM_GETMINMAXINFO` (`0x140785850` case 0x24)
scales the track sizes; the window procedure (`0x1407863a0`) divides incoming mouse coordinates by it
for both `WM_MOUSEMOVE` and every button message; `Window::getSize` (`0x140781cb0`) and
`Window::invalidateRect` (`0x140782ab0`) convert the other way; and the `WM_PAINT` flush
(`0x14077e1b0`) stretches the DIB from `surface / surfaceScale * dpiScale`, falling back to a 1:1
`SetDIBitsToDevice` when those cancel. Every one of those sites reads the same gate byte first:
`*(char*)(appObject + 0x49)`, where `appObject = *(void**)(AppModule + 0x10) - 8`.

FM8 leaves the gate at zero and never calls `SetProcessDpiAwareness`, so the scale is always 1.0 and
the whole layer is dead code in the shipped build. FM8.plus detours the three functions above: the app
object getter sets the gate byte on the way out (every reader calls it immediately before reading the
byte, so there is no startup ordering to get right), the DPI scale returns the chosen GUI Scale, and
the surface scale is held at 1 so the DIB stays at logical size and the `StretchDIBits` in the flush
does all the work. FM8 has no high-resolution artwork to supersample from, so the stock `ceil()`
factor would only enlarge the surface without enlarging what is drawn into it.

The blit itself is preceded by FM8's only `SetStretchBltMode` call, `SetStretchBltMode(hdc, HALFTONE)`, one call site per binary (`0x14077e1b0` / `0x180735240` / `0x180745630`, the WM_PAINT flush). HALFTONE interpolates, so an enlarged GUI comes out soft; FM8.plus swaps that entry in FM8's own GDI32 import table for one that passes `COLORONCOLOR`, which replicates pixels. At 1x the flush takes the `SetDIBitsToDevice` branch and never calls it, so a stock instance sharing the module is untouched.

The detours are process-wide, so `Core::addScaledWindow` records the editor window each FM8.plus shim is
given and the scale getter answers 1.0 for anything outside that window tree, leaving plain FM8
instances that share the module completely stock. The standalone registers nothing and scales its
whole process, dialogs included.

## 3. Open questions and blocked features

1. **VST3 CC1 apply-site (blocks VST3 mod-wheel-to-morph).** VST3 host remaps CC1/ch0 to param id 0x6d69646b, so `0x1800f3720` never sees a raw CC1. The parameter apply-path for 0x6d69646b in FM8.vst3 was not traced. Needed before CC1-to-morph works under a VST3 host.
2. **VST3 event-out ring producer (blocks VST3 arp MIDI-out).** The outbound Event ring at procThis+0xc50/+0xc58 is flushed by process but has no visible producer and is of unknown allocation. Safest is a process detour writing data.outputEvents directly rather than assuming the ring exists.
3. **`0x1800e7250` 3rd-arg semantics (affects arp-emit timing accuracy).** Absolute vs sub-block-relative sample offset was inferred; the decompiler dropped the explicit arg at the call site. Confirm with a live breakpoint that R8 equals the current sample offset.
4. **Morph `thread` arg + vtable+0xa8 transform (affects safe off-audio-thread morph writes).** For thread!=0 the setter remaps the value before storing. Determine which value the DAW passes and whether calling with thread=1 + pre-scaled 0..1 off the audio thread is safe, or whether a lock/queue is expected.
5. **VST2 index-136 to arp-On binding (affects programmatic arp-On via param path).** The route-2/sub-index-0 binding is inferred; descriptor builder `0x1801331c0` was not fully parsed.
6. **EXE input-sink wiring (affects MIDI-in injection confidence).** The concrete class binding a WinMidiInputDevice to the engine input queue (device+8 sink, virtual at +0x18) was not located. Event convergence is established by layout; the binding function is not.
7. **NoteOffPositionLI sub-field offsets (medium confidence).** Inferred from the pointer walk in `0x1801370a0`, not cross-checked against the node ctor. StepPositionLI map is high-confidence; NoteOffPositionLI is medium.
8. **Byte3 flag semantics at elem+0x13 (0x20 vs 0x10).** Likely realtime/system vs channel marker; irrelevant for note/CC capture.

## 4. How to re-verify (q.py)

```
# Arp emit core
python tools/q.py vst2 fn 0x1801370a0      # step generator: +0x4848 reset, +0x4840 tail
python tools/q.py vst2 fn 0x180135510      # MidiEventArray append: count@+8, stride 0x28
python tools/q.py vst2 fn 0x1800e7250      # run+dispatch drain loop (lVar2 += 0x28)
python tools/q.py vst2 callers 0x1800e6660 # handler callers (router + arp feed)

# Routing / On gate
python tools/q.py vst2 fn 0x180138c30      # filter: Clock+0x14 gate, Split block
python tools/q.py vst2 fn 0x18013b1e0      # sync run state

# Morph
python tools/q.py vst2 fn 0x1800dee10      # setParameterByTag: +0x68+tag*4, +0x3955
python tools/q.py vst2 grep 'Audio and Midi Settings'   # not morph; sanity of grep anchors
python tools/q.py vst2 fn 0x1800e3dd0      # index 21..25 -> tags 0x84..0x88

# CC handler + refutations
python tools/q.py vst2 fn 0x1800e6660      # CC1 case 1 -> engine+0x3988
python tools/q.py vst2 fn 0x18054bf30      # CONFIRM this is SQLite serial decode, NOT MIDI
python tools/q.py vst2 fn 0x1806ce420      # CONFIRM queue iterator, NOT CC handler

# VST2 MIDI-out (VST2 only)
python tools/q.py vst2 grep 'midiOutShortMsg'   # expect 0 hits (not here)
python tools/q.py vst2 fn 0x1802ea500      # audioMaster opcode 8 sender
python tools/q.py vst2 fn 0x1802e79f0      # VstMidiEvent builder (type=1, byteSize 0x18)

# VST3 buses (VST3 only)
python tools/q.py vst3 fn 0x1800c7920      # process: outputEvents flush at +0xc50/+0xc58
python tools/q.py vst3 fn 0x18016da80      # getBusCount: event-out list at +0xc8
python tools/q.py vst3 xrefs 0x180a36900   # process/bus vtable slot refs

# EXE hardware MIDI-out (standalone only)
python tools/q.py exe grep 'midiOutShortMsg'    # expect only 0x140196f60
python tools/q.py exe fn 0x140196f60       # Port send
python tools/q.py exe fn 0x14019ba70       # device enumerate (midiOutGetNumDevs)

# UI menu (all three)
python tools/q.py vst2 fn 0x18011dd00      # menu builder
python tools/q.py vst2 fn 0x180113240      # command sink (controlId 0x18)

# GUI scale (all three)
python tools/q.py exe fn 0x140779cc0       # app object getter (gate byte at +0x49)
python tools/q.py exe fn 0x140780500       # GetDpiForWindow / 96
python tools/q.py exe fn 0x140781630       # ceil(scale): the DIB supersample factor
python tools/q.py exe fn 0x1407856a0       # WM_PAINT: writes gfx+0x40 and gfx+0x44
python tools/q.py exe fn 0x14077e1b0       # flush: SetDIBitsToDevice 1:1 vs StretchDIBits
python tools/q.py exe fn 0x1407863a0       # window proc: mouse coordinates divided by the scale
python tools/q.py exe fn 0x1407844f0       # Window::setSize: logical size x scale -> SetWindowPos
```