// FM8.plus hook addresses for the two FM8 builds it supports.
//
// 1.4.6 of 2022-12-23 comes from docs/hooks.md, 1.4.1 of 2015-10-20 from docs/hooks-141.md.
// These are RVAs: add the runtime module base, which already accounts for ASLR. Neither build
// will receive another update, so they are stable forever. Do not edit by hand, regenerate from
// the maps the two hook docs describe.
//
// Image bases, for turning a static VA in either doc back into these RVAs: FM8.exe 0x140000000,
// FM8.dll and FM8.vst3 0x180000000, and the 1.4.1 32-bit FM8.dll 0x10000000.
#pragma once
#include <cstdint>

namespace fm8plus {

// What a shim knows about itself before it has looked at the module.
enum class Host { Exe, Vst2, Vst3 };

// Which binary we are actually inside. Host plus PE timestamp resolves to exactly one of these.
// The x86 entry only ever resolves in the 32-bit build, but it costs nothing to carry here and
// keeps one table for every target.
enum class Bin { Exe146, Vst2_146, Vst3_146, Exe141, Vst2_141, Vst2_141_x86, Count };
constexpr int kBinCount = (int)Bin::Count;

// One hook or callable, as an RVA per binary. 0 means "not present in that one".
struct Site {
    uint32_t v[kBinCount];
};

constexpr uint32_t rva(const Site& s, Bin b) { return s.v[(int)b]; }

// PE TimeDateStamp per binary. 1.4.6 stamps all three the same, 1.4.1 gives each its own.
constexpr uint32_t kTimeDateStamp[kBinCount] = {
    0x63a57e00, 0x63a57e00, 0x63a57e00,   // 1.4.6 EXE, VST2, VST3
    0x56266040, 0x56266059, 0x56265f2b,   // 1.4.1 EXE, VST2 x64, VST2 x86
};

// Resolve a loaded module to a Bin. False means this is an FM8 we do not map, and the caller
// must leave it stock rather than guess: a wrong table would detour arbitrary code.
constexpr bool resolveBin(Host h, uint32_t stamp, Bin& out) {
    for (int i = 0; i < kBinCount; ++i) {
        if (kTimeDateStamp[i] != stamp) continue;
        const Bin b = (Bin)i;
        const bool hostOk = (h == Host::Exe)  ? (b == Bin::Exe146 || b == Bin::Exe141)
                          : (h == Host::Vst3) ? (b == Bin::Vst3_146)
                          : (b == Bin::Vst2_146 || b == Bin::Vst2_141 || b == Bin::Vst2_141_x86);
        if (hostOk) { out = b; return true; }
    }
    return false;
}

// True for the 2015 build, whose NI::UIA has no HiDPI layer at all (see scaleLegacy in fm8plus.cpp).
constexpr bool is141(Bin b) {
    return b == Bin::Exe141 || b == Bin::Vst2_141 || b == Bin::Vst2_141_x86;
}

// Image bases, for turning a static VA in the hook docs back into these RVAs.
constexpr uint64_t kBaseExe  = 0x140000000ull;
constexpr uint64_t kBaseVst2 = 0x180000000ull;
constexpr uint64_t kBaseVst3 = 0x180000000ull;
constexpr uint64_t kBaseVst2x86 = 0x10000000ull;

// ---- MIDI event core (shared engine, all three binaries) -------------------
// Values are (VA - imagebase). e.g. VST2 0x1801370a0 - 0x180000000 = 0x1370a0.
constexpr Site kEngineStepGen      = {0x147740, 0x1370a0, 0x143bf0, 0x0e7c70, 0x0dc2b0, 0x0aa5b0}; // MidiEventArray* f(Engine*, int inBlockPos)
constexpr Site kEngineEmitNoteOn   = {0x147260, 0x136bc0, 0x143710, 0x0e78a0, 0x0dbee0, 0x000000}; // f(Engine*, uint voice, StepPositionLI*, Template*)
constexpr Site kMidiArrayPush      = {0x145bb0, 0x135510, 0x142060, 0x0e6c50, 0x0db290, 0x000000}; // f(MidiEventArray*, Template*)
constexpr Site kArpRunDispatch     = {0x0fd9c0, 0x0e7250, 0x0f4310, 0x0a1570, 0x094370, 0x0639c0}; // f(DspCore*, u32 destSel=2, int inBlockPos)  <-- OBSERVE/SUPPRESS point
constexpr Site kMidiEventHandler   = {0x0fcdd0, 0x0e6660, 0x0f3720, 0x0a08f0, 0x0936f0, 0x062d60}; // f(FM8Midi*, MidiEvent*, int flag=2)  <-- CC1 convergence
constexpr Site kArpEngineWrapper   = {0x1476f0, 0x137050, 0x143ba0, 0x0e7c60, 0x0dc2a0, 0x0aa5a0}; // MidiEventArray* f(ArpHolder*, int inBlockPos)
constexpr Site kNoteEventRouter    = {0x0e3050, 0x0cc370, 0x0d9580, 0x088e40, 0x07b990, 0x04ee40}; // f(FM8*, MidiEvent*, u32, u32)
constexpr Site kPerBlockProcessor  = {0x15f4e0, 0x14ee40, 0x15b890, 0x0ffd90, 0x0f4210, 0x0c14d0}; // f(SoundModule*, float* in..., int nFrames, char headless)
constexpr Site kTimeSortedQueueIt  = {0x842fc0, 0x6ce420, 0x6de5b0, 0x000000, 0x000000, 0x000000}; // char f(seq, u64* cursor, int key, u64* out, int* outKey)

// ---- Arp routing / state ---------------------------------------------------
constexpr Site kArpNoteFilter      = {0x1492d0, 0x138c30, 0x145690, 0x0e9ba0, 0x0de160, 0x0abee0}; // char f(Arp*, MidiEvent*, int off) -> 1 if consumed  <-- divert hook
constexpr Site kArpSyncRunState    = {0x14b880, 0x13b1e0, 0x147c30, 0x0ebbc0, 0x0e0060, 0x000000}; // f(Engine*)  start/stop transport from On flag
constexpr Site kArpParamSetter     = {0x144ef0, 0x134850, 0x1413a0, 0x0e5ee0, 0x0da520, 0x0a8cb0}; // bool f(Arp*, int idx, float v, int mode)
constexpr Site kArpDataFieldWriter = {0x145020, 0x134980, 0x1414d0, 0x000000, 0x000000, 0x000000}; // bool f(char* data, int idx, float v, char flag)

// ---- Morph setter (Morph X = tag 0x84, Morph Y = tag 0x85) -----------------
constexpr Site kVstSetParameter    = {0x10a860, 0x0f98c0, 0x1065e0, 0x0ac050, 0x0a0170, 0x06eca0}; // f(VstObj*, uint index, float norm, uint thread)
constexpr Site kParamDescLookup    = {0x0eeb00, 0x0d7e20, 0x0e5030, 0x0936c0, 0x086210, 0x057010}; // u32* f(int index)
constexpr Site kParamDescBuilder   = {0x0fa540, 0x0e3dd0, 0x0f0e90, 0x000000, 0x090940, 0x000000}; // bool f(int index, u64* outRec)  idx 21..25 -> tag 0x84..0x88
constexpr Site kEngineSetDispatch  = {0x0f6330, 0x0df7d0, 0x0ec9e0, 0x099c70, 0x08c7c0, 0x05da80}; // intptr f(EditBuffer*, u32 valueBits, u32* desc, uint thread, char flag)
constexpr Site kSetParameterByTag  = {0x0f5970, 0x0dee10, 0x0ec020, 0x099630, 0x08c180, 0x05d2b0}; // intptr f(EditBuffer*, uint tag, float value, char thread)  <-- THE morph setter
constexpr Site kRawValueStore      = {0x100100, 0x0e9b30, 0x0f6d50, 0x0997d0, 0x08c320, 0x000000}; // f(EditBuffer*, int tag, float valueBits)  (do not call directly)

// ---- VST2 MIDI-out (VST2 only) ---------------------------------------------
constexpr Site kVst2SendVstEvents  = {0, 0x2ea500, 0, 0x000000, 0x102890, 0x000000}; // bool f(HostAdapter*, VstEvents*)  audioMaster opcode 8
constexpr Site kVst2MidiEventBuild = {0, 0x2e79f0, 0, 0x000000, 0x056e70, 0x000000}; // f(InterfaceVST*, MidiRecord*)  vtable slot 5 / +0x28
constexpr Site kCcEmitter          = {0x0f5370, 0x0de810, 0x0eba20, 0x099160, 0x08bcb0, 0x05ce50}; // f(FM8Midi*, uint cc, uint value, char hiRes)

// ---- VST2 process / instantiation (VST2 only) ------------------------------
constexpr Site kVstPluginMain      = {0, 0x0bd090, 0, 0x000000, 0x053a80, 0x02b120}; // AEffect* f(audioMasterCallback)
constexpr Site kNICreatePlugin     = {0, 0x0bcf20, 0, 0x000000, 0x0539a0, 0x02b040}; // AEffect* f(host, int uid, int, int=2)
constexpr Site kDispatcherWrapper  = {0, 0x0bce70, 0, 0x000000, 0x000000, 0x000000}; // VstIntPtr f(AEffect*, int op, ...)
constexpr Site kAudioEffectXCtor   = {0, 0x2e8660, 0, 0x000000, 0x1009f0, 0x000000}; // f(this, host, int numPrograms, int numParams)
// processReplacing trampoline. This is a bare code label in padding (not a Ghidra function),
// so do NOT MinHook it by RVA. Capture the real pointer from AEffect+0x78 at runtime and wrap it.
constexpr Site kProcessReplacing   = {0, 0x2ea480, 0, 0x000000, 0x000000, 0x000000}; // f(AEffect*, float** in, float** out, int frames)
constexpr Site kAEffectDispatcher  = {0, 0x2e8f60, 0, 0x000000, 0x1012c0, 0x000000}; // VstIntPtr f(AEffect*, int op, int idx, VstIntPtr val, void* ptr, float opt)

// ---- VST3 buses (VST3 only) ------------------------------------------------
constexpr Site kVst3CreateInstance = {0, 0, 0x0bc830, 0x000000, 0x000000, 0x000000}; // tresult f(factory, int cidTag, FUnknown** args, void* ctx)
constexpr Site kVst3Process        = {0, 0, 0x0c7920, 0x000000, 0x000000, 0x000000}; // f(IAudioProcessor*, ProcessData*)
constexpr Site kVst3GetBusCount    = {0, 0, 0x16da80, 0x000000, 0x000000, 0x000000}; // int32 f(this, MediaType, BusDirection)  IComponent slot 7
constexpr Site kVst3GetBusInfo     = {0, 0, 0x0c5540, 0x000000, 0x000000, 0x000000}; // tresult f(this, MediaType, BusDirection, int32 idx, BusInfo&)  slot 8
constexpr Site kVst3ActivateBus    = {0, 0, 0x0c3b80, 0x000000, 0x000000, 0x000000}; // tresult f(this, MediaType, BusDirection, int32 idx, TBool)  slot 10
constexpr Site kVst3EventBusReg    = {0, 0, 0x16d4b0, 0x000000, 0x000000, 0x000000}; // f(componentBase, wchar* name, int channelCount, int)

// VST3 static vtable pointers (.rdata VAs, base 0x180000000).
constexpr uint64_t kVst3VtblPrimary        = 0x180a36800; // FM8VST3PlugIn primary
constexpr uint64_t kVst3VtblIComponent     = 0x180a36ae8;
constexpr uint64_t kVst3VtblIAudioProcessor= 0x180a36b60; // process slot at 0x180a36ba8
constexpr uint64_t kVst3VtblIMidiMapping   = 0x180a36d48;
constexpr uint32_t kVst3ProcessSlotRva     = 0xa36ba8;    // patch target for the process detour

// VST3 param id the host maps CC1 (mod wheel) to, via IMidiMapping.
constexpr uint32_t kVst3ModWheelParamId = 0x6d69646b;

// ---- EXE hardware MIDI out (standalone only) -------------------------------
constexpr Site kExePortSend        = {0x196f60, 0, 0, 0x2f9540, 0x000000, 0x000000}; // bool f(WinMidiPortOutputDevice*, MidiEventArray*)  midiOutShortMsg
constexpr Site kExePortOpen        = {0x195730, 0, 0, 0x2f93b0, 0x000000, 0x000000}; // bool f(WinMidiPortOutputDevice*)
constexpr Site kExeStreamSend      = {0x197030, 0, 0, 0x2f95e0, 0x000000, 0x000000}; // f(WinMidiStreamOutputDevice*, MidiEventArray*)
constexpr Site kExeDriverEnumerate = {0x19ba70, 0, 0, 0x2f9e70, 0x000000, 0x000000}; // f(WinMidiDriver*)

// ---- GUI scale: NI::UIA's own HiDPI path (docs/gui-runtime.md 4) -----------
// FM8 already sizes its window, maps mouse coordinates, scales its dirty rects and stretches the
// final DIB blit by a per-window "DPI scale". Nothing ever turns it on: FM8 never calls
// SetProcessDpiAwareness, so GetDpiForWindow always answers 96 and one byte in the NI::UIA app
// object gates the whole path off. Detouring these three makes the scale ours.
constexpr Site kUiaAppObject = {0x779cc0, 0x731140, 0x741530, 0x47e510, 0x45a290, 0x362f40}; // void* f(void)      app object; byte +0x49 gates HiDPI
constexpr Site kUiaDpiScale  = {0x780500, 0x737b00, 0x747ef0, 0x000000, 0x000000, 0x000000}; // float f(HWND)      GetDpiForWindow / 96
constexpr Site kUiaSurfScale = {0x781630, 0x738aa0, 0x748e90, 0x000000, 0x000000, 0x000000}; // float f(Window*)   ceil(dpi scale): DIB supersample factor
constexpr uint32_t kUiaHiDpiFlag = 0x49;   // byte offset of the gate in the app object

// ---- UI menu (FormMain, all three) -----------------------------------------
constexpr Site kMenuBuilder        = {0x12e310, 0x11dd00, 0x12a890, 0x0cf330, 0x0c3810, 0x093cf0}; // f(FormMain*)
constexpr Site kMenuCommandSink    = {0x123960, 0x113240, 0x11fdd0, 0x0c4020, 0x0b8400, 0x08a6a0}; // f(FormMain+0x248*, int ctrlId, int notify, EventData*)
constexpr Site kPopupAddItem       = {0x753f10, 0x70b4d0, 0x71b920, 0x469a80, 0x445b40, 0x3530f0}; // int f(PopupMenu*, char* long, char* short, int cmdId)
constexpr Site kPopupAddSeparator  = {0x754410, 0x70b9d0, 0x71be20, 0x46a090, 0x4460c0, 0x353510}; // f(PopupMenu*)
constexpr Site kPopupSetSubmenu    = {0x75fe50, 0x717380, 0x7277d0, 0x000000, 0x000000, 0x000000}; // bool f(PopupMenu*, int idx, PopupMenu* sub)
constexpr Site kPopupSetCheckState = {0x75fd30, 0x717260, 0x7276b0, 0x4761d0, 0x452180, 0x000000}; // bool f(PopupMenu*, int idx, int state)
// What FM8's own logo click does: the FormMain sink's `case 5` calls this with
// *(FM8VstObject + 0x5620), and it builds and runs FM8AboutDialog (ResourceManager 8000/8000).
constexpr Site kShowAboutDialog    = {0x12bac0, 0x11b4b0, 0x128040, 0x0bdcf0, 0x0b20f0, 0x084ef0}; // f(FM8App*)  modal, UI thread only

// ---- Per-build structure layout --------------------------------------------
// Everything the detours dereference at runtime. These differ between builds, and a wrong value is
// a crash on the audio thread rather than a cosmetic bug, so each field is either verified against
// the decompilation or the capability that needs it is switched off.
//
// The one that bites: the MidiEvent packed word holds [data2][data1][status] in 1.4.6 and
// [status][data1][data2] in 1.4.1, confirmed from each build's own MIDI handler, which masks the
// data bytes with 0x7f and switches on (status & 0xf0).
struct Layout {
    uint32_t coreVstObj;     // DspCore -> FM8VstObject (pointer-sized, so it halves on x86)
    uint32_t vstObjEditBuf;  // FM8VstObject -> FM8EditBuffer
    uint32_t elemWord;       // MidiEvent -> packed status/data word
    bool     statusFirst;    // true = [status][data1][data2] (1.4.1 order)
    uint32_t arrCount;       // MidiEventArray -> count
    uint32_t arrData;        // MidiEventArray -> element base
    uint32_t arrStride;      // MidiEventArray element stride
    uint32_t vstObjApp;      // FM8VstObject -> FM8App, 0 when it is not a fixed offset
    bool     midiVerified;   // false: arp and morph stay off, the offsets are not confirmed
};

constexpr Layout kLayout[kBinCount] = {
    // 1.4.6 EXE / VST2 / VST3, from docs/hooks.md.
    {0x08, 0x55d0, 0x10, false, 0x08, 0x10, 0x28, 0x5620, true},
    {0x08, 0x55d0, 0x10, false, 0x08, 0x10, 0x28, 0x5620, true},
    {0x08, 0x55d0, 0x10, false, 0x08, 0x10, 0x28, 0x5620, true},
    // 1.4.1 x64. The EditBuffer accessor returns *(obj+0x55a0), not 0x55d0. The About argument is
    // reached through a vtable chain here rather than a fixed field, so it is 0 and About stays
    // hidden until that path is traced.
    {0x08, 0x55a0, 0x10, true, 0x08, 0x10, 0x28, 0, true},
    {0x08, 0x55a0, 0x10, true, 0x08, 0x10, 0x28, 0, true},
    // 1.4.1 x86, read from the arp dispatch's disassembly rather than the decompiler, whose
    // __thiscall argument recovery is unreliable on this binary:
    //   MOV ECX,[EAX+0x4] ; CALL acc -> MOV EAX,[ECX+0x5454] ; MOV ECX,[EAX+0x2994] ; CALL wrapper
    // then the drain loop reads count at +0x4, data at +0x8 and steps 0x20.
    {0x04, 0x5454, 0x10, true, 0x04, 0x08, 0x20, 0, true},
};

// Compile-time self-check. The resolver has to send each (host, stamp) pair to its own build and
// reject anything else, and no build may claim verified MIDI offsets without an EditBuffer offset
// to walk: that pointer is dereferenced on the audio thread.
namespace detail {
constexpr Bin resolved(Host h, uint32_t s) { Bin b = Bin::Count; return resolveBin(h, s, b) ? b : Bin::Count; }
constexpr bool layoutSane() {
    for (int i = 0; i < kBinCount; ++i)
        if (kLayout[i].midiVerified && kLayout[i].vstObjEditBuf == 0) return false;
    return true;
}
}
static_assert(detail::resolved(Host::Exe,  0x63a57e00) == Bin::Exe146,       "1.4.6 EXE");
static_assert(detail::resolved(Host::Vst2, 0x63a57e00) == Bin::Vst2_146,     "1.4.6 VST2");
static_assert(detail::resolved(Host::Vst3, 0x63a57e00) == Bin::Vst3_146,     "1.4.6 VST3");
static_assert(detail::resolved(Host::Exe,  0x56266040) == Bin::Exe141,       "1.4.1 EXE");
static_assert(detail::resolved(Host::Vst2, 0x56266059) == Bin::Vst2_141,     "1.4.1 VST2 x64");
static_assert(detail::resolved(Host::Vst2, 0x56265f2b) == Bin::Vst2_141_x86, "1.4.1 VST2 x86");
static_assert(detail::resolved(Host::Exe,  0x56266059) == Bin::Count, "a VST2 stamp is not an EXE");
static_assert(detail::resolved(Host::Vst2, 0xdeadbeef) == Bin::Count, "an unmapped FM8 must be refused");
static_assert(detail::layoutSane(), "a build claims verified MIDI offsets but has no EditBuffer offset");

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
