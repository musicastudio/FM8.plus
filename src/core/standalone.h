#pragma once
#include <windows.h>

// Standalone attach for FM8.plus. The launcher (FM8.plus.exe) starts the real FM8.exe suspended and
// injects FM8.plus.dll; that DLL's DllMain calls attachExe when it detects it is inside FM8.exe.
// This replaces the old version.dll sideload, so plain FM8.exe is left completely untouched.
namespace fm8plus {
namespace standalone {

// Shift the FM8 logo now (we are injected while the process is still suspended, before the GUI is
// built), then defer the hooks, WinMM MIDI-out, and overlay onto a worker thread. `self` is the
// injected module handle. Safe to call once from DllMain.
void attachExe(HMODULE self);

}  // namespace standalone
}  // namespace fm8plus
