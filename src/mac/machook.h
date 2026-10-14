// macOS plumbing for FM8.plus: find FM8's image, resolve its internals by symbol name, and redirect
// them. NI ships the Mac binaries with their full C++ symbol table (local symbols included), so
// nothing here is an address: every target is looked up by its mangled name at load time, which
// covers the VST2, VST3 and AU builds and both architectures with one table.
#pragma once
#include <cstddef>

namespace fm8plus::mac {

// The mach_header of the loaded image containing `anyAddressInIt`, or null.
const void* imageOf(const void* anyAddressInIt);

// Resolve each of `names` (mangled, with the leading underscore as stored in the symbol table) in
// `image` into `out`. Returns how many were found; missing ones are left null.
int resolve(const void* image, const char* const* names, void** out, int count);

// Point `image`'s own imports of `symbol` (mangled, with the leading underscore) at `to`: the
// symbol pointers its stubs jump through, a data write like the vtable hooks. Only that image's
// calls change. `to` calls the real function by name. False when the image imports no such symbol.
bool rebindImport(const void* image, const char* symbol, void* to);

// Send every call of `from` to `to`, on threads that have called armThread(). A hardware
// breakpoint, not a code patch, so it holds in hardened hosts and on arm64. `to` replaces `from`
// outright: it can never call `from` (that would trap again), so it must do its whole job.
bool redirect(void* from, void* to);

// Put a call where there is none: a thread reaching `at` runs fn(r[args[0]], .. r[args[3]]), its
// registers at that point by number (arm64 xN; x86_64 in thread-state order: 1 rbx, 12..15 r12..r15),
// and fn's return lands on `resume`. Breakpointing an instruction inside a function rather than its
// entry means the trap fires only on the path that needs it.
bool redirectCall(void* at, void* fn, void* resume, const unsigned char (&args)[4]);

// Load the redirects into this thread's debug registers. Cheap after the first call; audio
// wrappers call it at the top of every process call, since hosts move work between threads.
bool armThread();

} // namespace fm8plus::mac
