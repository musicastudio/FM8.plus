// FM8.plus hook addresses for the FM8 build of 2022-12-23 (PE TimeDateStamp 0x63a57e00).
//
// Generated from docs/hooks.md (the verified reverse-engineering result). These are static
// virtual addresses in each binary; subtract the image base to get an RVA, then add the
// runtime module base (which already accounts for ASLR). Do not edit by hand; regenerate
// with tools/find_rvas.py if the analysis changes. The binaries never receive another
// update, so these are stable forever.
//
// Image bases: FM8.dll (VST2) and FM8.vst3 = 0x180000000, FM8.exe = 0x140000000.
#pragma once
#include <cstdint>

namespace fm8plus {

enum class Bin { Exe, Vst2, Vst3 };

// One hook or callable, given as the static VA in each binary (0 = not present there).
struct Site {
    uint32_t exe;   // RVA within FM8.exe   (VA - 0x140000000)
    uint32_t vst2;  // RVA within FM8.dll   (VA - 0x180000000)
    uint32_t vst3;  // RVA within FM8.vst3  (VA - 0x180000000)
};

// Helper: pick the RVA for a binary; 0 means "not applicable to this host".
constexpr uint32_t rva(const Site& s, Bin b) {
    return b == Bin::Exe ? s.exe : b == Bin::Vst2 ? s.vst2 : s.vst3;
}

// Image bases, for turning a static VA in docs/hooks.md back into these RVAs.
constexpr uint64_t kBaseExe  = 0x140000000ull;
constexpr uint64_t kBaseVst2 = 0x180000000ull;
constexpr uint64_t kBaseVst3 = 0x180000000ull;

// ---- MIDI event core (shared engine, all three binaries) -------------------
// Values are (VA - imagebase). e.g. VST2 0x1801370a0 - 0x180000000 = 0x1370a0.
constexpr Site kEngineStepGen      = {0x147740, 0x1370a0, 0x143bf0}; // MidiEventArray* f(Engine*, int inBlockPos)
constexpr Site kEngineEmitNoteOn   = {0x147260, 0x136bc0, 0x143710}; // f(Engine*, uint voice, StepPositionLI*, Template*)
constexpr Site kMidiArrayPush      = {0x145bb0, 0x135510, 0x142060}; // f(MidiEventArray*, Template*)
constexpr Site kArpRunDispatch     = {0x0fd9c0, 0x0e7250, 0x0f4310}; // f(DspCore*, u32 destSel=2, int inBlockPos)  <-- OBSERVE/SUPPRESS point
constexpr Site kMidiEventHandler   = {0x0fcdd0, 0x0e6660, 0x0f3720}; // f(FM8Midi*, MidiEvent*, int flag=2)  <-- CC1 convergence
constexpr Site kArpEngineWrapper   = {0x1476f0, 0x137050, 0x143ba0}; // MidiEventArray* f(ArpHolder*, int inBlockPos)
constexpr Site kNoteEventRouter    = {0x0e3050, 0x0cc370, 0x0d9580}; // f(FM8*, MidiEvent*, u32, u32)
constexpr Site kPerBlockProcessor  = {0x15f4e0, 0x14ee40, 0x15b890}; // f(SoundModule*, float* in..., int nFrames, char headless)
constexpr Site kTimeSortedQueueIt  = {0x842fc0, 0x6ce420, 0x6de5b0}; // char f(seq, u64* cursor, int key, u64* out, int* outKey)

// ---- Arp routing / state ---------------------------------------------------
constexpr Site kArpNoteFilter      = {0x1492d0, 0x138c30, 0x145690}; // char f(Arp*, MidiEvent*, int off) -> 1 if consumed  <-- divert hook
constexpr Site kArpSyncRunState    = {0x14b880, 0x13b1e0, 0x147c30}; // f(Engine*)  start/stop transport from On flag
constexpr Site kArpParamSetter     = {0x144ef0, 0x134850, 0x1413a0}; // bool f(Arp*, int idx, float v, int mode)
constexpr Site kArpDataFieldWriter = {0x145020, 0x134980, 0x1414d0}; // bool f(char* data, int idx, float v, char flag)

// ---- Morph setter (Morph X = tag 0x84, Morph Y = tag 0x85) -----------------
constexpr Site kVstSetParameter    = {0x10a860, 0x0f98c0, 0x1065e0}; // f(VstObj*, uint index, float norm, uint thread)
constexpr Site kParamDescLookup    = {0x0eeb00, 0x0d7e20, 0x0e5030}; // u32* f(int index)
constexpr Site kParamDescBuilder   = {0x0fa540, 0x0e3dd0, 0x0f0e90}; // bool f(int index, u64* outRec)  idx 21..25 -> tag 0x84..0x88
constexpr Site kEngineSetDispatch  = {0x0f6330, 0x0df7d0, 0x0ec9e0}; // intptr f(EditBuffer*, u32 valueBits, u32* desc, uint thread, char flag)
constexpr Site kSetParameterByTag  = {0x0f5970, 0x0dee10, 0x0ec020}; // intptr f(EditBuffer*, uint tag, float value, char thread)  <-- THE morph setter
constexpr Site kRawValueStore      = {0x100100, 0x0e9b30, 0x0f6d50}; // f(EditBuffer*, int tag, float valueBits)  (do not call directly)

// ---- VST2 MIDI-out (VST2 only) ---------------------------------------------
constexpr Site kVst2SendVstEvents  = {0, 0x2ea500, 0}; // bool f(HostAdapter*, VstEvents*)  audioMaster opcode 8
constexpr Site kVst2MidiEventBuild = {0, 0x2e79f0, 0}; // f(InterfaceVST*, MidiRecord*)  vtable slot 5 / +0x28
constexpr Site kCcEmitter          = {0x0f5370, 0x0de810, 0x0eba20}; // f(FM8Midi*, uint cc, uint value, char hiRes)

// ---- VST2 process / instantiation (VST2 only) ------------------------------
constexpr Site kVstPluginMain      = {0, 0x0bd090, 0}; // AEffect* f(audioMasterCallback)
constexpr Site kNICreatePlugin     = {0, 0x0bcf20, 0}; // AEffect* f(host, int uid, int, int=2)
constexpr Site kDispatcherWrapper  = {0, 0x0bce70, 0}; // VstIntPtr f(AEffect*, int op, ...)
constexpr Site kAudioEffectXCtor   = {0, 0x2e8660, 0}; // f(this, host, int numPrograms, int numParams)
// processReplacing trampoline. This is a bare code label in padding (not a Ghidra function),
// so do NOT MinHook it by RVA. Capture the real pointer from AEffect+0x78 at runtime and wrap it.
constexpr Site kProcessReplacing   = {0, 0x2ea480, 0}; // f(AEffect*, float** in, float** out, int frames)
constexpr Site kAEffectDispatcher  = {0, 0x2e8f60, 0}; // VstIntPtr f(AEffect*, int op, int idx, VstIntPtr val, void* ptr, float opt)

// ---- VST3 buses (VST3 only) ------------------------------------------------
constexpr Site kVst3CreateInstance = {0, 0, 0x0bc830}; // tresult f(factory, int cidTag, FUnknown** args, void* ctx)
constexpr Site kVst3Process        = {0, 0, 0x0c7920}; // f(IAudioProcessor*, ProcessData*)
constexpr Site kVst3GetBusCount    = {0, 0, 0x16da80}; // int32 f(this, MediaType, BusDirection)  IComponent slot 7
constexpr Site kVst3GetBusInfo     = {0, 0, 0x0c5540}; // tresult f(this, MediaType, BusDirection, int32 idx, BusInfo&)  slot 8
constexpr Site kVst3ActivateBus    = {0, 0, 0x0c3b80}; // tresult f(this, MediaType, BusDirection, int32 idx, TBool)  slot 10
constexpr Site kVst3EventBusReg    = {0, 0, 0x16d4b0}; // f(componentBase, wchar* name, int channelCount, int)

// VST3 static vtable pointers (.rdata VAs, base 0x180000000).
constexpr uint64_t kVst3VtblPrimary        = 0x180a36800; // FM8VST3PlugIn primary
constexpr uint64_t kVst3VtblIComponent     = 0x180a36ae8;
constexpr uint64_t kVst3VtblIAudioProcessor= 0x180a36b60; // process slot at 0x180a36ba8
constexpr uint64_t kVst3VtblIMidiMapping   = 0x180a36d48;
constexpr uint32_t kVst3ProcessSlotRva     = 0xa36ba8;    // patch target for the process detour

// VST3 param id the host maps CC1 (mod wheel) to, via IMidiMapping.
constexpr uint32_t kVst3ModWheelParamId = 0x6d69646b;

// ---- EXE hardware MIDI out (standalone only) -------------------------------
constexpr Site kExePortSend        = {0x196f60, 0, 0}; // bool f(WinMidiPortOutputDevice*, MidiEventArray*)  midiOutShortMsg
constexpr Site kExePortOpen        = {0x195730, 0, 0}; // bool f(WinMidiPortOutputDevice*)
constexpr Site kExeStreamSend      = {0x197030, 0, 0}; // f(WinMidiStreamOutputDevice*, MidiEventArray*)
constexpr Site kExeDriverEnumerate = {0x19ba70, 0, 0}; // f(WinMidiDriver*)

// ---- GUI scale: NI::UIA's own HiDPI path (docs/gui-runtime.md 4) -----------
// FM8 already sizes its window, maps mouse coordinates, scales its dirty rects and stretches the
// final DIB blit by a per-window "DPI scale". Nothing ever turns it on: FM8 never calls
// SetProcessDpiAwareness, so GetDpiForWindow always answers 96 and one byte in the NI::UIA app
// object gates the whole path off. Detouring these three makes the scale ours.
constexpr Site kUiaAppObject = {0x779cc0, 0x731140, 0x741530}; // void* f(void)      app object; byte +0x49 gates HiDPI
constexpr Site kUiaDpiScale  = {0x780500, 0x737b00, 0x747ef0}; // float f(HWND)      GetDpiForWindow / 96
constexpr Site kUiaSurfScale = {0x781630, 0x738aa0, 0x748e90}; // float f(Window*)   ceil(dpi scale): DIB supersample factor
constexpr uint32_t kUiaHiDpiFlag = 0x49;   // byte offset of the gate in the app object

// ---- UI menu (FormMain, all three) -----------------------------------------
constexpr Site kMenuBuilder        = {0x12e310, 0x11dd00, 0x12a890}; // f(FormMain*)
constexpr Site kMenuCommandSink    = {0x123960, 0x113240, 0x11fdd0}; // f(FormMain+0x248*, int ctrlId, int notify, EventData*)
constexpr Site kPopupAddItem       = {0x753f10, 0x70b4d0, 0x71b920}; // int f(PopupMenu*, char* long, char* short, int cmdId)
constexpr Site kPopupAddSeparator  = {0x754410, 0x70b9d0, 0x71be20}; // f(PopupMenu*)
constexpr Site kPopupSetSubmenu    = {0x75fe50, 0x717380, 0x7277d0}; // bool f(PopupMenu*, int idx, PopupMenu* sub)
constexpr Site kPopupSetCheckState = {0x75fd30, 0x717260, 0x7276b0}; // bool f(PopupMenu*, int idx, int state)
// What FM8's own logo click does: the FormMain sink's `case 5` calls this with
// *(FM8VstObject + 0x5620), and it builds and runs FM8AboutDialog (ResourceManager 8000/8000).
constexpr Site kShowAboutDialog    = {0x12bac0, 0x11b4b0, 0x128040}; // f(FM8App*)  modal, UI thread only

// ---- Selected structure field offsets (from docs/hooks.md) -----------------
// Engine (NI::MIDIArpeggiator_Engine)
constexpr uint32_t kEngMidiArray   = 0x4840; // MidiEventArray
constexpr uint32_t kEngOutCount    = 0x4848; // reset each position by the step generator
constexpr uint32_t kEngPlaymode    = 0x4878;
constexpr uint32_t kEngClock       = 0x4928; // *(Engine+0x4928)+0x14 == live "running" gate
// MidiEventArray
constexpr uint32_t kArrCount       = 0x08;
constexpr uint32_t kArrData        = 0x10;
constexpr uint32_t kArrEnd         = 0x18;
constexpr uint32_t kArrElemStride  = 0x28;
constexpr uint32_t kElemPackedWord = 0x10; // [data2][data1][status][flags] little-endian
// FM8VstObject / FM8EditBuffer
constexpr uint32_t kVstObjEditBuf  = 0x55d0; // *(VstObj+0x55d0) = FM8EditBuffer*
constexpr uint32_t kVstObjApp      = 0x5620; // *(VstObj+0x5620) = FM8App*, the About dialog's argument
constexpr uint32_t kEbParamBase    = 0x68;   // param float array, indexed by tag
constexpr uint32_t kEbMorphX       = 0x278;  // == kEbParamBase + 0x84*4
constexpr uint32_t kEbMorphY       = 0x27c;  // == kEbParamBase + 0x85*4
constexpr uint32_t kEbVtblSetByTag = 0x10;   // vtable slot for setParameterByTag
// Morph tags
constexpr uint32_t kTagMorphX = 0x84, kTagMorphY = 0x85, kTagMorphRndX = 0x86, kTagMorphRndY = 0x87, kTagMorphSeed = 0x88;
// Arp object graph
constexpr uint32_t kCoreEditBuf    = 0x55d0; // FM8 core -> FM8EditBuffer (same as VstObj here)
constexpr uint32_t kEditBufArp     = 0x29e8; // FM8EditBuffer -> FM8MIDIArpeggiator
constexpr uint32_t kArpEngine      = 0x20;   // FM8MIDIArpeggiator -> Engine
constexpr uint32_t kClockRunFlag   = 0x14;   // byte at *(Engine+0x4928)+0x14
// FM8Midi
constexpr uint32_t kMidiController1 = 0x71d, kMidiController2 = 0x71e;
constexpr uint32_t kMidiDumpCtrls   = 0x1c51, kMidiSendChannel = 0x1c64;

// PE identity guard.
constexpr uint32_t kFm8TimeDateStamp = 0x63a57e00;

} // namespace fm8plus
