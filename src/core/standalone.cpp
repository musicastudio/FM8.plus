// Standalone attach implementation. Installs the shared arp/morph hooks against the FM8.exe image,
// opens a WinMM MIDI-out port, and shows the overlay. Formerly the body of the version.dll shim;
// now driven by launcher injection so nothing is placed in FM8's own folder.
#include "standalone.h"
#include <mmsystem.h>
#include <string>
#include "fm8plus.h"
#include "settings.h"
#include "ui.h"

#pragma comment(lib, "winmm.lib")

namespace fm8plus {
namespace standalone {
namespace {

HMODULE g_self = nullptr;
InstanceState g_inst;
HMIDIOUT g_midiOut = nullptr;
ui::Overlay g_overlay;

// Open the MIDI-out device whose name matches the INI (else device 0).
void openMidiOut() {
    std::wstring want = settings::midiOutDevice();
    UINT n = midiOutGetNumDevs();
    UINT pick = 0;
    if (!want.empty()) {
        for (UINT i = 0; i < n; ++i) {
            MIDIOUTCAPSW caps{};
            if (midiOutGetDevCapsW(i, &caps, sizeof caps) == MMSYSERR_NOERROR && want == caps.szPname) { pick = i; break; }
        }
    }
    if (n > 0) midiOutOpen(&g_midiOut, pick, 0, 0, CALLBACK_NULL);
}

// End of every arp dispatch: send queued events to the WinMM port and apply the pending morph.
void onArpBlock(InstanceState& st) {
    if (g_midiOut) {
        for (int i = 0; i < st.outCount; ++i) {
            const MidiMsg& m = st.outBuf[i];
            DWORD msg = m.status | (m.data1 << 8) | (m.data2 << 16);
            midiOutShortMsg(g_midiOut, msg);
        }
    }
    st.clearBlock();
    Core::applyPendingMorphInternal(st);
}

DWORD WINAPI overlayThread(LPVOID) {
    g_overlay.setStandalone(true);
    g_overlay.attachToMainWindow(&g_inst, g_self, 30000);  // wait up to 30s for FM8's window
    return 0;
}

// Heavy init, off the loader lock. Runs even while the process is still suspended (this thread is
// not the suspended main thread); MinHook patches code FM8 only executes after the launcher resumes.
DWORD WINAPI initThread(LPVOID) {
    settings::load(g_self);
    g_inst.morphCc.store((int16_t)settings::morphCcDefault());
    g_inst.arpMode.store((uint8_t)settings::arpModeDefault());
    g_inst.morphRadius.store(settings::morphRadius());
    g_inst.morphStartDeg.store(settings::morphStartDeg());
    void* base = GetModuleHandleW(nullptr);          // FM8.exe image base
    if (!Core::install(base, Bin::Exe)) return 0;    // build mismatch -> plain FM8, features off
    Core::setSingleton(&g_inst);
    Core::setArpBlockCallback(&onArpBlock);
    openMidiOut();
    CloseHandle(CreateThread(nullptr, 0, overlayThread, nullptr, 0, nullptr));
    return 0;
}

}  // namespace

void attachExe(HMODULE self) {
    g_self = self;
    // Must happen before FM8 builds its GUI. We are injected while FM8.exe is suspended, so this runs
    // first; guarded, so a wrong build is left untouched.
    if (!Core::serveForms(GetModuleHandleW(nullptr))) Core::shiftLogoLeft(GetModuleHandleW(nullptr), 11);
    CloseHandle(CreateThread(nullptr, 0, initThread, nullptr, 0, nullptr));
}

}  // namespace standalone
}  // namespace fm8plus
