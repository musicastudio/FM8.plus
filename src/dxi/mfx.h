// Cakewalk MFX / DXi interop declarations: the interface IDs, method order and struct layouts a
// DXi soft synth must match to be driven by a DXi host. Written from the published interface
// layout (the same ABI FM8 1.0.3's own DXi answered); only what FM8.plus uses is declared.
#pragma once
#include <windows.h>
#include <unknwn.h>

namespace mfx {

inline const GUID IID_IMfxSoftSynth     = {0x5bbdf239, 0x3046, 0x4daa, {0x8a, 0xdd, 0x3b, 0x93, 0x73, 0x54, 0x5b, 0x74}};
inline const GUID IID_IMfxSoftSynth2    = {0x6e88453f, 0x4a85, 0x4159, {0xb6, 0x58, 0x88, 0xc4, 0x3d, 0x28, 0x82, 0x87}};
inline const GUID IID_IMfxNotify        = {0x753b2366, 0x6cd2, 0x4e61, {0x99, 0x9b, 0x91, 0x28, 0xfc, 0xdb, 0x7b, 0xcd}};
inline const GUID IID_IMfxInputPort     = {0x49c2ee08, 0x7dca, 0x4df3, {0x9a, 0x41, 0x35, 0xcf, 0xdb, 0x44, 0xf1, 0x2e}};
inline const GUID IID_IMfxTempoMap      = {0xff76f7c3, 0xf166, 0x11d1, {0xa8, 0xe0, 0x00, 0x00, 0xa0, 0x09, 0x0d, 0xaf}};
inline const GUID IID_IMfxTimeConverter = {0xff94aba0, 0x4df9, 0x4120, {0x94, 0x7d, 0x5e, 0x30, 0x3e, 0x49, 0xb8, 0x1f}};
inline const GUID IID_IMfxEventQueue    = {0xff76f7c2, 0xf166, 0x11d1, {0xa8, 0xe0, 0x00, 0x00, 0xa0, 0x09, 0x0d, 0xaf}};
inline const GUID IID_IMfxDataQueue     = {0x7a37a621, 0x1b1b, 0x11d2, {0xa8, 0xe0, 0x00, 0x00, 0xa0, 0x09, 0x0d, 0xaf}};

typedef long Channel;   // MFX_CHANNEL: the host track feeding the synth

#pragma pack(push, 1)
// One raw MIDI short message, as delivered to OnInput (live playing).
struct Data {
    LONG time;                      // host ticks ("now" for live input)
    BYTE status, data1, data2, pad;
};

// One musical event, as delivered to OnEvents (playback). A Note carries its own duration, so there
// is no separate note-off.
struct Event {
    enum Type : int { Note, KeyAft, Control, Patch, ChanAft, Wheel, RPN, NRPN, Sysx, Text, Lyric,
                      MuteMask, VelOfs, VelTrim, KeyOfs, KeyTrim, ShortMsg };
    LONG time;                      // host ticks
    BYTE port, chan;                // (MuteMask: set/clear bits; VelOfs etc.: first byte is the amount)
    Type type;
    union {
        struct { BYTE key, vel, velOff; DWORD duration; } note;   // duration in ticks
        struct { BYTE key, amt; } keyAft;
        struct { BYTE num, val; } control;
        struct { BYTE patch, bankSelMethod; short bank; } patch;
        struct { BYTE amt; } chanAft;
        struct { short val; } wheel;                               // -8192..8191
        struct { WORD num, val; } rpn;
        DWORD shortMsg;
        Channel channel;
    } u;
};
#pragma pack(pop)
static_assert(sizeof(Data) == 8 && sizeof(Event) == 17, "MFX struct layout");

struct NotifyMsg {
    enum Type : int { ChannelMidiChannel, ChannelMuteMask, ChannelVelOfs, ChannelVelTrim,
                      ChannelKeyOfs, ChannelKeyTrim, PortListChanged };
    Type type;
    Channel channel;
    char a, b;                      // MidiChannel / Ofs / Trim in a; MuteMask: set in a, clear in b
};

enum TimeFormat : int { TF_NULL, TF_SECONDS, TF_SAMPLES, TF_TICKS };
struct Time {
    TimeFormat format;
    union { double seconds; LONGLONG samples; LONG ticks; };
};

struct IEventQueue : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Add(const Event&) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCount(int* n) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetAt(int ix, Event* e) = 0;
};
struct IDataQueue : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Add(const Data&) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCount(int* n) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetAt(int ix, Data* d) = 0;
};
struct ITempoMap : IUnknown {
    virtual LONG STDMETHODCALLTYPE TicksToMsecs(LONG ticks) = 0;
    virtual LONG STDMETHODCALLTYPE MsecsToTicks(LONG ms) = 0;
    virtual int  STDMETHODCALLTYPE GetTicksPerQuarterNote() = 0;
    virtual int  STDMETHODCALLTYPE GetTempoIndexForTime(LONG ticks) = 0;
    virtual int  STDMETHODCALLTYPE GetTempoCount() = 0;
    virtual HRESULT STDMETHODCALLTYPE GetTempoAt(int ix, LONG* ticks, int* bpm100) = 0;
};
struct ITimeConverter : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE ConvertMfxTime(Time* t, TimeFormat to) = 0;
};

// What the synth implements. Method order is the ABI.
struct ISoftSynth : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Connect(IUnknown* context) = 0;
    virtual HRESULT STDMETHODCALLTYPE Disconnect() = 0;
    virtual HRESULT STDMETHODCALLTYPE OnStart(LONG time) = 0;
    virtual HRESULT STDMETHODCALLTYPE OnLoop(LONG restart, LONG stop) = 0;
    virtual HRESULT STDMETHODCALLTYPE OnStop(LONG time) = 0;
    virtual HRESULT STDMETHODCALLTYPE OnEvents(LONG from, LONG thru, Channel ch, IEventQueue* in) = 0;
    virtual HRESULT STDMETHODCALLTYPE OnInput(Channel ch, IDataQueue* in) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetBanksForPatchNames(int** banks, int* count) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetIsDrumPatch(int bank, int patch) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetIsDiatonicNoteNames(int bank, int patch) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPatchNames(int bank, IUnknown** list) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetNoteNames(int bank, int patch, IUnknown** list) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetControllerNames(IUnknown** list) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetRpnNames(IUnknown** list) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetNrpnNames(IUnknown** list) = 0;
};
struct ISoftSynth2 : ISoftSynth {
    virtual HRESULT STDMETHODCALLTYPE GetInstrument(int channel, IUnknown** instrument) = 0;
};
struct INotify : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE OnMfxNotify(NotifyMsg* msg) = 0;
};
struct IInputPort : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE SetInputCallback(IUnknown* cb) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetInputCallback(IUnknown** cb) = 0;
};

} // namespace mfx
