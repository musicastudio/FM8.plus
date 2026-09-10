// FM8.plus global settings, persisted to %APPDATA%\FM8.plus\FM8.plus.ini.
// Per-instance state (arp mode, mod-wheel toggle) travels in the DAW project via a chunk trailer;
// these are the process-wide defaults and knobs plus the standalone MIDI-out device name.
#pragma once
#include <string>
#include <windows.h>

namespace fm8plus::settings {

void load(HMODULE self);          // read the INI (call once at attach); records `self` for UI use
void save();                      // write current values back
HMODULE self();                   // module handle for resource/UI ownership

bool  defaultModWheelMorph();
void  setDefaultModWheelMorph(bool v);
int   arpModeDefault();            // 0 Internal, 1 Clone, 2 MIDI only (standalone + fresh instances)
void  setArpModeDefault(int m);
float morphRadius();
float morphStartDeg();
std::wstring midiOutDevice();     // standalone only; empty = first device
void setMidiOutDevice(const std::wstring& name);

} // namespace fm8plus::settings
