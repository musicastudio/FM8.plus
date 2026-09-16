// Serve replacement GUI resources to FM8 at load time, built from FM8's own.
//
// FM8 loads every form and picture through FindResourceA/SizeofResource/LoadResource/LockResource
// on its own module (docs/gui-runtime.md 1.1). Patching those four entries in the FM8 module's OWN
// import table redirects only FM8's lookups (the host DAW's imports are untouched), and a blob
// served from here is never written to the stock file, so a Native Access reinstall cannot break
// anything.
#pragma once
#include <windows.h>
#include <cstddef>

namespace fm8plus::Rsrc {

// Swap one import thunk in `mod`'s OWN import table, by DLL and function name. Returns the original
// pointer, or null if `mod` does not import that function. This is the mechanism behind everything
// here: only FM8's calls are redirected, and nothing on disk changes.
void* patchImport(HMODULE mod, const char* dll, const char* func, void* replacement);

// Hook the import table of `fm8` (FM8.exe / FM8.dll / FM8.vst3, already loaded). Idempotent.
bool install(HMODULE fm8);

// Build the "FM8+" wordmark out of FM8's own resources and serve it in their place: FRM 5 and 15
// with the wordmark control widened, and PICTURE 193 widened with the "+" drawn into the new space.
// Every byte is derived on this machine from the user's own FM8, so the binaries we ship carry no
// Native Instruments form data or artwork at all. Call after install(); false leaves FM8's GUI stock.
bool serveLogo();

} // namespace fm8plus::Rsrc
