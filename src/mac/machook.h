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

// Send every call of `from` to `to`, on threads that have called armThread(). A hardware
// breakpoint, not a code patch, so it holds in hardened hosts and on arm64. `to` replaces `from`
// outright: it can never call `from` (that would trap again), so it must do its whole job.
bool redirect(void* from, void* to);

// Load the redirects into this thread's debug registers. Cheap after the first call; audio
// wrappers call it at the top of every process call, since hosts move work between threads.
bool armThread();

} // namespace fm8plus::mac
