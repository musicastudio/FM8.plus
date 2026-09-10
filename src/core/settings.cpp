#include "settings.h"
#include <shlobj.h>
#include <cwchar>
#include <cstdlib>

namespace fm8plus::settings {
namespace {
HMODULE g_self = nullptr;
std::wstring g_iniPath;
int   g_morphCc = -1;
int   g_arpMode = 0;
float g_radius = 0.5f;
float g_startDeg = -90.0f;
std::wstring g_midiOut;

std::wstring iniPath() {
    wchar_t* base = nullptr;
    if (SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &base) != S_OK) return L"";
    std::wstring dir = std::wstring(base) + L"\\FM8.plus";
    CoTaskMemFree(base);
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\FM8.plus.ini";
}
} // namespace

void load(HMODULE self) {
    g_self = self;
    g_iniPath = iniPath();
    if (g_iniPath.empty()) return;
    const wchar_t* s = L"FM8.plus"; const wchar_t* p = g_iniPath.c_str();
    g_morphCc = (int)(short)GetPrivateProfileIntW(s, L"morph_cc", 0xffff, p);  // 0xffff -> -1 (off)
    if (g_morphCc < -1 || g_morphCc > 127) g_morphCc = -1;
    g_arpMode = (int)GetPrivateProfileIntW(s, L"arp_mode", 0, p);
    if (g_arpMode < 0 || g_arpMode > 2) g_arpMode = 0;
    wchar_t buf[64];
    GetPrivateProfileStringW(s, L"morph_radius", L"0.5", buf, 64, p);   g_radius = (float)_wtof(buf);
    GetPrivateProfileStringW(s, L"morph_start_deg", L"-90", buf, 64, p); g_startDeg = (float)_wtof(buf);
    wchar_t dev[256];
    GetPrivateProfileStringW(s, L"midi_out_device", L"", dev, 256, p);  g_midiOut = dev;
}

void save() {
    if (g_iniPath.empty()) return;
    const wchar_t* s = L"FM8.plus"; const wchar_t* p = g_iniPath.c_str();
    wchar_t cc[8]; swprintf(cc, 8, L"%d", g_morphCc); WritePrivateProfileStringW(s, L"morph_cc", cc, p);
    wchar_t mb[8]; swprintf(mb, 8, L"%d", g_arpMode); WritePrivateProfileStringW(s, L"arp_mode", mb, p);
    wchar_t buf[64];
    swprintf(buf, 64, L"%.4f", g_radius);   WritePrivateProfileStringW(s, L"morph_radius", buf, p);
    swprintf(buf, 64, L"%.1f", g_startDeg); WritePrivateProfileStringW(s, L"morph_start_deg", buf, p);
    WritePrivateProfileStringW(s, L"midi_out_device", g_midiOut.c_str(), p);
}

HMODULE self() { return g_self; }
int   morphCcDefault() { return g_morphCc; }
void  setMorphCcDefault(int cc) { g_morphCc = (cc < -1 || cc > 127) ? -1 : cc; save(); }
int   arpModeDefault() { return g_arpMode; }
void  setArpModeDefault(int m) { g_arpMode = (m < 0 || m > 2) ? 0 : m; save(); }
float morphRadius() { return g_radius; }
float morphStartDeg() { return g_startDeg; }
std::wstring midiOutDevice() { return g_midiOut; }
void setMidiOutDevice(const std::wstring& n) { g_midiOut = n; save(); }

} // namespace fm8plus::settings
