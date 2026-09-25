// FM8.plus shared core: per-instance state, the arp/morph feature logic, and the two
// internal FM8 detours that are identical across the standalone, VST2 and VST3 binaries.
//
// Threading: the arp hooks run on the audio thread inside a host's process callback. Each
// shim sets Core::current (thread_local) to the InstanceState it is about to process, so the
// hooks know which instance and mode they are serving. Feature toggles are std::atomic so the
// UI thread can flip them; the note masks and out-buffer are touched only on the audio thread.
#pragma once
#include <atomic>
#include <cmath>
#include <cstdint>
#include "rvas.h"

namespace fm8plus {

enum class ArpMode : uint8_t { Internal = 0, CloneToMidi = 1, MidiOnly = 2 };

// Host-tempo multiplier for the Tempo Override modes (1 .25x, 2 .5x, 3 2x, 4 4x; 0/5 = no scale).
inline double tempoFactor(uint8_t mode) {
    return mode == 1 ? 0.25 : mode == 2 ? 0.5 : mode == 3 ? 2.0 : mode == 4 ? 4.0 : 1.0;
}
// Extra output gain 0..10 dB as a linear multiplier.
inline float gainLinear(int8_t db) { return db > 0 ? (float)std::pow(10.0, db / 20.0) : 1.0f; }

// One MIDI message queued for the host, with its in-block sample offset.
struct MidiMsg {
    uint8_t status, data1, data2;
    int32_t offset;
};

// Two 16x128 note bitsets: which notes are sounding internally and which were sent to MIDI out.
struct NoteMask {
    uint32_t bits[16][4] = {};
    void set(int ch, int note)   { bits[ch & 15][(note & 127) >> 5] |=  (1u << (note & 31)); }
    void clear(int ch, int note) { bits[ch & 15][(note & 127) >> 5] &= ~(1u << (note & 31)); }
    bool test(int ch, int note) const { return bits[ch & 15][(note & 127) >> 5] & (1u << (note & 31)); }
    void reset() { for (auto& c : bits) for (auto& w : c) w = 0; }
};

// Per plugin instance (or the single standalone). Owned by a shim; pointer handed to the hooks
// through Core::current for the duration of one process call.
struct InstanceState {
    std::atomic<uint8_t> arpMode{(uint8_t)ArpMode::Internal};
    std::atomic<int16_t> morphCc{-1};       // -1 = off, else the MIDI CC number that rotates the morph
    std::atomic<uint8_t> morphPending{0xff}; // last value of that CC this block, 0xff = none
    std::atomic<uint8_t> tempoMode{0};      // 0 off, 1 .25x, 2 .5x, 3 2x, 4 4x, 5 custom (host tempo scale)
    std::atomic<int8_t>  gainDb{0};         // extra output gain 0..10 dB
    std::atomic<bool>    pendingFlush{false}; // set by the UI thread on mode change; the audio thread flushes

    // Audio-thread-only working set.
    static constexpr int kMaxOut = 1024;
    MidiMsg  outBuf[kMaxOut];
    int      outCount = 0;
    NoteMask intOn, extOn;

    // FM8 EditBuffer pointer, captured on the audio thread (used by the internal morph setter for
    // the standalone and VST3 paths). null until the engine has processed once.
    std::atomic<void*> editBuf{nullptr};

    // FM8App pointer, captured the same way and from the same object, for FM8's own About dialog.
    std::atomic<void*> appObj{nullptr};

    // Global knobs mirrored from settings (read on the audio thread).
    std::atomic<float> morphRadius{0.5f};
    std::atomic<float> morphStartDeg{-90.0f};

    void clearBlock() { outCount = 0; }
    void pushOut(uint8_t s, uint8_t d1, uint8_t d2, int32_t off) {
        if (outCount < kMaxOut) outBuf[outCount++] = {s, d1, d2, off};
    }
};

// The shared feature core: resolves hook addresses from a loaded FM8 module base, validates the
// build, installs the two arp detours, and exposes the routing decision the detours apply.
namespace Core {

// thread_local instance being processed right now (set by a shim around the real process call).
extern thread_local InstanceState* current;

// Single-instance fallback used when `current` is null (the standalone has one engine and no
// process wrapper to set the thread_local). Shims for hosted plugins leave this null.
void setSingleton(InstanceState* s);

// Optional callback fired at the end of each arp dispatch with the active instance. The standalone
// uses it to flush queued events to its WinMM port and apply morph every block; hosted shims drain
// in their own process wrapper and leave this unset.
void setArpBlockCallback(void (*cb)(InstanceState&));

// Validate the module at `base` is the expected FM8 build; returns false if the timestamp differs.
bool validateBuild(void* base, Host h);

// Resolve addresses from `base` and install the arp detours (idempotent). Returns false on any
// MinHook error or build mismatch; on false the caller should pass through to stock FM8.
bool install(void* base, Host host);

void uninstall();

// Feature 1 math: CC1 value 0..127 into a Morph X/Y in 0..1 on a circle.
void morphXYFromCc(const InstanceState& st, uint8_t cc1, float& x, float& y);

// Drive FM8's internal Morph X/Y setter with the captured EditBuffer (standalone/VST3 morph).
// Returns false if no EditBuffer has been captured yet. SEH-guarded against a bad pointer.
bool setMorphXY(InstanceState& st, float x, float y);

// Apply a pending mod-wheel value (st.lastCc1) via the internal setter; clears it. No-op if morph
// off, no CC pending, or no EditBuffer. Used by the standalone/VST3 per-block wrappers.
void applyPendingMorphInternal(InstanceState& st);

// Flush external note-offs for every sounding out-note (call on mode change / stop / close).
void flushExternal(InstanceState& st);

// Open FM8's own About dialog, the one its logo opens in stock FM8: the FormMain command sink's
// `case 5` (control 5 is the wordmark) calls one function with the FM8App pointer, and that is what
// this calls, so the dialog is FM8's, not a copy. `aboutReady` is false until the audio thread has
// run once, since the pointer is captured from the arp dispatch like the EditBuffer. UI thread only
// (the dialog is modal), and SEH-guarded against a stale pointer.
bool aboutReady(const InstanceState& st);
bool showAbout(InstanceState& st);

// Resolve a hook Site to an absolute address in the installed module (for shim-added hooks).
void* addressOf(const Site& s);

// GUI scale: 1.0 = stock FM8, up to 4.0 = everything four times the size. Drives NI::UIA's own
// HiDPI path (see kUiaDpiScale in rvas.h), so FM8 itself sizes the window, maps the mouse, scales its
// dirty rects and stretches the blit; we only supply the number. Safe to call before a window exists.
void  setGuiScale(float s);
float guiScale();

// Hosted only: the scale hooks are process-wide, but a DAW can have plain FM8 instances sharing the
// same module, and those must stay stock. Each shim registers the editor window the host gave it
// before FM8 builds its own window inside it; only that window tree is scaled. Registering anything
// turns the gate on, so the standalone (which never does) keeps scaling its whole process.
void addScaledWindow(void* hwnd);

// Arm the next window FM8 creates on this thread as a scaled editor (1.4.1 has no UIA layer to
// ask, so the shim says when the editor is coming).
void expectEditorWindow();

// False on a build whose MidiEvent/EditBuffer layout is not confirmed (1.4.1 x86 today): the arp
// and morph features are absent there and the menu must not offer them.
bool midiFeaturesAvailable();

// Build the "FM8+" wordmark out of `module`'s own resources and serve it back through its import
// table (see rsrc.h): the two header forms with the wordmark control widened, and the wordmark
// bitmap with the "+" drawn into the new space. FM8 then draws, scales and clips the "+" as part of
// its own logo, and nothing of Native Instruments' is shipped to get there. Must run BEFORE FM8
// builds its GUI: for the standalone that means DllMain (before WinMain); for the plugins, at load.
// False leaves FM8's GUI completely stock.
bool serveLogo(void* module);

// True once serveLogo has succeeded, i.e. the wordmark is the wider "FM8+" one. ui.cpp hit-tests
// clicks against the matching rect; without it the stock "FM8" logo itself opens the menu.
bool logoWidened();

} // namespace Core
} // namespace fm8plus
