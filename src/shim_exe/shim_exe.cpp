// FM8.plus standalone attach. Ships as version.dll beside FM8.exe. FM8.exe imports only four
// version.dll functions (all forwarded here to the real System32 version.dll) and version.dll is
// not a KnownDLL, so the app directory wins the load order and no byte of FM8.exe changes.
//
// On first use it installs the shared arp/morph hooks against the FM8.exe image and opens a WinMM
// MIDI-out port. Arp notes are sent with midiOutShortMsg at the block boundary; the mod wheel drives
// the internal Morph setter. Both features are gated by an INI written by the in-editor toggles.
#include <windows.h>
#include <mmsystem.h>
#include <mutex>
#include <string>
#include "../core/fm8plus.h"
#include "../core/settings.h"
#include "../core/ui.h"
#include "MinHook.h"

#pragma comment(lib, "winmm.lib")
using namespace fm8plus;

namespace {
HMODULE g_self = nullptr;
HMODULE g_realVersion = nullptr;   // System32\version.dll
InstanceState g_inst;
HMIDIOUT g_midiOut = nullptr;
std::once_flag g_initOnce;
ui::Overlay g_overlay;

FARPROC realVersion(const char* name) {
    if (!g_realVersion) {
        wchar_t sys[MAX_PATH]; GetSystemDirectoryW(sys, MAX_PATH);
        g_realVersion = LoadLibraryW((std::wstring(sys) + L"\\version.dll").c_str());
    }
    return g_realVersion ? GetProcAddress(g_realVersion, name) : nullptr;
}

// Open the MIDI-out device whose name matches the INI (else device 0). Called once at init.
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

// Fired at the end of every arp dispatch (see Core::setArpBlockCallback): send queued events to the
// WinMM port and apply any pending mod-wheel morph. Runs on the audio thread.
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

void initOnce() {
    settings::load(g_self);
    g_inst.modWheelMorph.store(settings::defaultModWheelMorph());
    g_inst.arpMode.store((uint8_t)settings::arpModeDefault());
    g_inst.morphRadius.store(settings::morphRadius());
    g_inst.morphStartDeg.store(settings::morphStartDeg());
    void* base = GetModuleHandleW(nullptr);     // FM8.exe image base
    if (!Core::install(base, Bin::Exe)) return; // build mismatch -> stay a pure forwarder
    Core::setSingleton(&g_inst);
    Core::setArpBlockCallback(&onArpBlock);
    openMidiOut();
    CloseHandle(CreateThread(nullptr, 0, overlayThread, nullptr, 0, nullptr));
}

void ensureInit() { std::call_once(g_initOnce, initOnce); }
} // namespace

// --- forwarded version.dll exports (each triggers lazy init on the way through) --------------
// Named Fwd_* here and exported under the real API names via version.def, so they do not collide
// with the winver.h declarations.
extern "C" {
DWORD __stdcall Fwd_GetFileVersionInfoSizeW(LPCWSTR f, LPDWORD h) {
    ensureInit();
    static auto p = (DWORD(__stdcall*)(LPCWSTR, LPDWORD))realVersion("GetFileVersionInfoSizeW");
    return p ? p(f, h) : 0;
}
BOOL __stdcall Fwd_GetFileVersionInfoW(LPCWSTR f, DWORD h, DWORD len, LPVOID data) {
    ensureInit();
    static auto p = (BOOL(__stdcall*)(LPCWSTR, DWORD, DWORD, LPVOID))realVersion("GetFileVersionInfoW");
    return p ? p(f, h, len, data) : FALSE;
}
BOOL __stdcall Fwd_VerQueryValueW(LPCVOID b, LPCWSTR q, LPVOID* out, PUINT len) {
    ensureInit();
    static auto p = (BOOL(__stdcall*)(LPCVOID, LPCWSTR, LPVOID*, PUINT))realVersion("VerQueryValueW");
    return p ? p(b, q, out, len) : FALSE;
}
BOOL __stdcall Fwd_VerQueryValueA(LPCVOID b, LPCSTR q, LPVOID* out, PUINT len) {
    ensureInit();
    static auto p = (BOOL(__stdcall*)(LPCVOID, LPCSTR, LPVOID*, PUINT))realVersion("VerQueryValueA");
    return p ? p(b, q, out, len) : FALSE;
}
} // extern "C"

BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) { g_self = h; DisableThreadLibraryCalls(h); }
    return TRUE;
}
