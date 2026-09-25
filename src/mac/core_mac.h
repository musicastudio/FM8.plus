// macOS additions to the shared Core API in core/fm8plus.h. The Mac core resolves FM8 by symbol
// name (machook.h) instead of the Windows RVA tables, so it needs a few entry points of its own.
#pragma once
#include "../core/fm8plus.h"

namespace fm8plus::Core {

// Install against the FM8 image that contains `anyAddressInFm8` (e.g. its VSTPluginMain). Safe to
// call repeatedly. False when the image is not an FM8 1.4.6 we can map, which leaves FM8 stock.
bool installMac(const void* anyAddressInFm8);

// Tie an instance to its FM8 engine object (class FM8), so the logo click and About know whose they
// are. `root` is any object the host wrapper holds (AEffect::object, the AU or VST3 instance); the
// FM8 object is found by walking pointers from it and checking vtables, no layout assumed.
bool bindInstance(InstanceState* st, void* root);
void unbindInstance(InstanceState* st);

// Called on the UI thread when the FM8 wordmark of a bound instance is clicked, in place of FM8's
// own About panel. `st` is the clicked instance.
void setLogoHandler(void (*fn)(InstanceState* st));

// Call at the top of every audio process call, before FM8 runs: loads the arp redirect into this
// thread's debug registers (a no-op once it holds them).
void armAudioThread();

// Mac About: FM8's own panel for the bound instance. UI thread only.
bool showAboutMac(InstanceState* st);

// Serve the "FM8+" wordmark from `rsrcPath` (the FM8 bundle's Resources/FM8.rsrc) through FM8's own
// in-memory resource map. Call before every editor open: FM8 empties the map when its app module
// shuts down with the last instance.
bool serveLogoMac(const char* rsrcPath);

} // namespace fm8plus::Core
