// FM8.plus settings on macOS: the same keys as the Windows INI, in /Users/Shared/FM8.plus, the one
// folder every user and every host can write without an installer step (the Mac's ProgramData).
#include "../core/settings.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <sys/stat.h>

namespace fm8plus::settings {
namespace {
HMODULE g_self = nullptr;
const char kDir[] = "/Users/Shared/FM8.plus";
const char kIni[] = "/Users/Shared/FM8.plus/FM8.plus.ini";
int   g_morphCc = -1;
int   g_arpMode = 0;
float g_scale = 1.0f;
float g_radius = 0.5f;
float g_startDeg = -90.0f;
std::wstring g_midiOut;
} // namespace

void load(HMODULE self) {
    g_self = self;
    FILE* f = fopen(kIni, "r");
    if (!f) return;
    std::map<std::string, std::string> kv;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        char* eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        std::string v(eq + 1);
        while (!v.empty() && (v.back() == '\n' || v.back() == '\r')) v.pop_back();
        kv[line] = v;
    }
    fclose(f);
    auto num = [&](const char* k, double d) { auto it = kv.find(k); return it == kv.end() ? d : atof(it->second.c_str()); };
    g_morphCc = (int)num("morph_cc", -1);
    if (g_morphCc < -1 || g_morphCc > 127) g_morphCc = -1;
    g_arpMode = (int)num("arp_mode", 0);
    if (g_arpMode < 0 || g_arpMode > 2) g_arpMode = 0;
    g_scale = (float)num("gui_scale", 1);
    if (!(g_scale >= 1.0f) || g_scale > 4.0f) g_scale = 1.0f;
    g_radius = (float)num("morph_radius", 0.5);
    g_startDeg = (float)num("morph_start_deg", -90);
    auto it = kv.find("midi_out_device");
    if (it != kv.end()) g_midiOut.assign(it->second.begin(), it->second.end());
}

void save() {
    mkdir(kDir, 0777);
    chmod(kDir, 0777);   // shared by every user, as %ProgramData%\FM8.plus is on Windows
    FILE* f = fopen(kIni, "w");
    if (!f) return;
    fprintf(f, "[FM8.plus]\nmorph_cc=%d\narp_mode=%d\ngui_scale=%.2f\nmorph_radius=%.4f\nmorph_start_deg=%.1f\n",
            g_morphCc, g_arpMode, g_scale, g_radius, g_startDeg);
    fprintf(f, "midi_out_device=%s\n", std::string(g_midiOut.begin(), g_midiOut.end()).c_str());
    fclose(f);
    chmod(kIni, 0666);
}

HMODULE self() { return g_self; }
int   morphCcDefault() { return g_morphCc; }
void  setMorphCcDefault(int cc) { g_morphCc = (cc < -1 || cc > 127) ? -1 : cc; save(); }
int   arpModeDefault() { return g_arpMode; }
void  setArpModeDefault(int m) { g_arpMode = (m < 0 || m > 2) ? 0 : m; save(); }
float guiScale() { return g_scale; }
void  setGuiScale(float v) { g_scale = (!(v >= 1.0f) || v > 4.0f) ? 1.0f : v; save(); }
float morphRadius() { return g_radius; }
float morphStartDeg() { return g_startDeg; }
std::wstring midiOutDevice() { return g_midiOut; }
void setMidiOutDevice(const std::wstring& n) { g_midiOut = n; save(); }

} // namespace fm8plus::settings
