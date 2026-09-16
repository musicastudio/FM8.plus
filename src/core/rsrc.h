// Serve replacement GUI resources (forms and artwork rebuilt by tools/gen_forms.py) to FM8 at
// load time.
//
// FM8 loads every form and picture through FindResourceA/SizeofResource/LoadResource/LockResource
// on its own module (docs/gui-runtime.md 1.1). Patching those four entries in the FM8 module's OWN import table
// redirects only FM8's lookups (the host DAW's imports are untouched), and a blob served from here is
// never written to the stock file, so a Native Access reinstall cannot break anything.
#pragma once
#include <windows.h>
#include <cstddef>

namespace fm8plus::Rsrc {

// Hook the import table of `fm8` (FM8.exe / FM8.dll / FM8.vst3, already loaded). Idempotent.
bool install(HMODULE fm8);

// Serve `data` for resource `type`/`id` instead of the one in the file ("FRM", "PICTURE", ...).
// Call after install; the bytes must stay valid for the life of the process (point at a static array
// from tools/gen_forms.py). `type` must be a string literal.
void serve(const char* type, int id, const void* data, size_t size);

} // namespace fm8plus::Rsrc
