// FM8.plus shared core: per-instance state, the arp/morph feature logic, and the two
// internal FM8 detours that are identical across the standalone, VST2 and VST3 binaries.
//
// Threading: the arp hooks run on the audio thread inside a host's process callback. Each
// shim sets Core::current (thread_local) to the InstanceState it is about to process, so the
// hooks know which instance and mode they are serving. Feature toggles are std::atomic so the
// UI thread can flip them; the note masks and out-buffer are touched only on the audio thread.
#pragma once
#include <atomic>
#include <cstdint>
#include "rvas.h"

namespace fm8plus {

enum class ArpMode : uint8_t { Internal = 0, CloneToMidi = 1, MidiOnly = 2 };

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
    std::atomic<bool>    modWheelMorph{false};
    std::atomic<uint8_t> lastCc1{0xff};   // 0xff = none seen this block (VST2/EXE morph)
    std::atomic<bool>    pendingFlush{false}; // set by the UI thread on mode change; the audio thread flushes

    // Audio-thread-only working set.
    static constexpr int kMaxOut = 1024;
    MidiMsg  outBuf[kMaxOut];
    int      outCount = 0;
    NoteMask intOn, extOn;

    // FM8 EditBuffer pointer, captured on the audio thread (used by the internal morph setter for
    // the standalone and VST3 paths). null until the engine has processed once.
    std::atomic<void*> editBuf{nullptr};

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
bool validateBuild(void* base);

// Resolve addresses from `base` and install the arp detours (idempotent). Returns false on any
// MinHook error or build mismatch; on false the caller should pass through to stock FM8.
bool install(void* base, Bin which);

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

// Resolve a hook Site to an absolute address in the installed module (for shim-added hooks).
void* addressOf(const Site& s);

} // namespace Core
} // namespace fm8plus
