// Minimal native VST2 host probe, for the targets tools/vsteditor.py cannot reach.
//
// The Python harness is ctypes and the only Python on this machine is 64-bit, so the 32-bit shim
// (FM8 1.4.1 x86, the last 32-bit plugin NI shipped) has no way to be tested from it. This does
// the same few things natively: load the plug-in, open its editor in a real window, pump, then
// report the rect and dump the window to a BMP.
//
//     vst2probe <plugin.dll> [out.bmp] [seconds]     open the editor, dump it
//     vst2probe <plugin.dll> --arp                    drive the arpeggiator and count MIDI out
//
// Exit code 0 when the editor opened and the child window matched the reported rect.
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

// Just enough of the VST2 ABI to open an editor. Field offsets are the ABI's, not a guess.
struct AEffect;
using DispatcherFn = intptr_t (*)(AEffect*, int32_t op, int32_t idx, intptr_t val, void* ptr, float opt);
struct AEffect {
    int32_t magic;
    DispatcherFn dispatcher;
    void* processDeprecated;
    void* setParameter;
    void* getParameter;
    int32_t numPrograms, numParams, numInputs, numOutputs, flags;
    intptr_t resvd1, resvd2;
    int32_t initialDelay, realQualitiesDeprecated, offQualitiesDeprecated;
    float ioRatioDeprecated;
    void* object; void* user;
    int32_t uniqueID, version;
    void (*processReplacing)(AEffect*, float**, float**, int32_t);
    void* processDoubleReplacing;
    char future[56];
};
struct ERect { int16_t top, left, bottom, right; };
struct VstMidiEvent {
    int32_t type, byteSize, deltaFrames, flags, noteLength, noteOffset;
    char midiData[4], detune, noteOffVelocity, reserved1, reserved2;
};
struct VstEvents { int32_t numEvents; intptr_t reserved; VstMidiEvent* events[8]; };

enum { effOpen = 0, effClose = 1, effSetSampleRate = 10, effSetBlockSize = 11, effMainsChanged = 12,
       effEditGetRect = 13, effEditOpen = 14, effEditClose = 15, effProcessEvents = 25,
       effGetParamName = 8 };

// What the plug-in sends back out. audioMasterProcessEvents (opcode 8) is the arp MIDI-out path.
int g_eventsFromPlugin = 0;
int g_noteOnsFromPlugin = 0;
char g_firstEvents[8][3];
int g_firstCount = 0;

intptr_t VSTCALLBACK_host(AEffect*, int32_t opcode, int32_t, intptr_t, void* ptr, float) {
    if (opcode == 1 /*audioMasterVersion*/) return 2400;
    if (opcode == 8 /*audioMasterProcessEvents*/ && ptr) {
        auto* evs = (VstEvents*)ptr;
        for (int i = 0; i < evs->numEvents && i < 8; ++i) {
            VstMidiEvent* e = evs->events[i];
            if (!e || e->type != 1) continue;
            ++g_eventsFromPlugin;
            const unsigned char st = (unsigned char)e->midiData[0];
            if ((st & 0xf0) == 0x90 && e->midiData[2] != 0) ++g_noteOnsFromPlugin;
            if (g_firstCount < 8) {
                memcpy(g_firstEvents[g_firstCount], e->midiData, 3);
                ++g_firstCount;
            }
        }
        return 1;
    }
    return 0;
}

LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_CLOSE) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(h, m, w, l);
}

void pump(double seconds) {
    const DWORD until = GetTickCount() + (DWORD)(seconds * 1000);
    MSG msg;
    while (GetTickCount() < until) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(10);
    }
}

// PrintWindow into a DIB and write a 32-bit BMP. Only ever captures the probe window.
bool dump(HWND h, const char* path) {
    RECT r{}; GetClientRect(h, &r);
    const int w = r.right, ht = r.bottom;
    if (w <= 0 || ht <= 0) return false;
    HDC dc = GetDC(h), mem = CreateCompatibleDC(dc);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -ht;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(mem, bmp);
    PrintWindow(h, mem, PW_CLIENTONLY);
    const uint32_t stride = (uint32_t)w * 4, size = stride * (uint32_t)ht;
    BITMAPFILEHEADER fh{};
    fh.bfType = 0x4d42;
    fh.bfOffBits = sizeof(fh) + sizeof(BITMAPINFOHEADER);
    fh.bfSize = fh.bfOffBits + size;
    FILE* f = fopen(path, "wb");
    bool ok = false;
    if (f) {
        fwrite(&fh, sizeof(fh), 1, f);
        fwrite(&bi.bmiHeader, sizeof(BITMAPINFOHEADER), 1, f);
        fwrite(bits, size, 1, f);
        fclose(f);
        ok = true;
    }
    SelectObject(mem, old); DeleteObject(bmp); DeleteDC(mem); ReleaseDC(h, dc);
    return ok;
}

} // namespace

// Find a parameter by name, so the Arp On index is looked up rather than hard-coded.
int paramIndex(AEffect* eff, const char* want) {
    // 256 bytes, not the 64 the spec implies: hosts pass 256 and FM8 writes as much as it likes.
    char name[256];
    for (int i = 0; i < eff->numParams; ++i) {
        memset(name, 0, sizeof(name));
        eff->dispatcher(eff, effGetParamName, i, 0, name, 0);
        if (_stricmp(name, want) == 0) return i;
    }
    return -1;
}

// Hold three notes through the arpeggiator and see what the plug-in sends the host.
int runArp(AEffect* eff) {
    const int kBlock = 512;
    eff->dispatcher(eff, effSetSampleRate, 0, 0, nullptr, 44100.0f);
    eff->dispatcher(eff, effSetBlockSize, 0, kBlock, nullptr, 0);
    eff->dispatcher(eff, effMainsChanged, 0, 1, nullptr, 0);

    // FM8 calls it "Arpeggiator On"; the shorter name is what some builds report.
    int arpOn = paramIndex(eff, "Arp On");
    if (arpOn < 0) arpOn = paramIndex(eff, "Arpeggiator On");
    printf("Arp On param index: %d\n", arpOn);
    if (arpOn < 0) { printf("RESULT: could not find the arpeggiator parameter\n"); return 1; }
    if (arpOn >= 0) ((void(*)(AEffect*, int32_t, float))eff->setParameter)(eff, arpOn, 1.0f);

    VstMidiEvent ev[3]{};
    VstEvents evs{};
    const unsigned char notes[3] = {60, 64, 67};
    for (int i = 0; i < 3; ++i) {
        ev[i].type = 1; ev[i].byteSize = sizeof(VstMidiEvent);
        ev[i].midiData[0] = (char)0x90; ev[i].midiData[1] = (char)notes[i]; ev[i].midiData[2] = 100;
        evs.events[i] = &ev[i];
    }
    evs.numEvents = 3;
    eff->dispatcher(eff, effProcessEvents, 0, 0, &evs, 0);

    static float inL[512], inR[512], outL[512], outR[512];
    float* in[2] = {inL, inR};
    float* out[2] = {outL, outR};
    float peak = 0.0f;
    for (int b = 0; b < 200; ++b) {
        memset(outL, 0, sizeof(outL)); memset(outR, 0, sizeof(outR));
        eff->processReplacing(eff, in, out, kBlock);
        for (int i = 0; i < kBlock; ++i) {
            const float a = outL[i] < 0 ? -outL[i] : outL[i];
            if (a > peak) peak = a;
        }
    }
    eff->dispatcher(eff, effMainsChanged, 0, 0, nullptr, 0);
    printf("audio peak %.3f; MIDI events from plugin: %d (note-ons %d)\n",
           peak, g_eventsFromPlugin, g_noteOnsFromPlugin);
    for (int i = 0; i < g_firstCount; ++i)
        printf("  %02x %02x %02x\n", (unsigned char)g_firstEvents[i][0],
               (unsigned char)g_firstEvents[i][1], (unsigned char)g_firstEvents[i][2]);
    const bool ok = g_noteOnsFromPlugin > 0 && peak > 0.0f;
    printf("RESULT: %s\n", ok ? "arpeggiator emitted MIDI and audio" : "NO ARP MIDI OUT");
    return ok ? 0 : 1;
}

// Drive the Morph Rotate Control: the configured CC should sweep Morph X/Y around a circle while
// FM8 itself never sees the CC. Needs morph_cc set in FM8.plus.ini (this passes CC 11).
int runMorph(AEffect* eff) {
    const int kBlock = 512, kMorphX = 21, kMorphY = 22, kCc = 11;
    eff->dispatcher(eff, effSetSampleRate, 0, 0, nullptr, 44100.0f);
    eff->dispatcher(eff, effSetBlockSize, 0, kBlock, nullptr, 0);
    eff->dispatcher(eff, effMainsChanged, 0, 1, nullptr, 0);
    auto get = (float(*)(AEffect*, int32_t))eff->getParameter;

    static float inL[512], inR[512], outL[512], outR[512];
    float* in[2] = {inL, inR};
    float* out[2] = {outL, outR};
    float xs[5], ys[5];
    const int vals[5] = {0, 32, 64, 96, 127};
    for (int k = 0; k < 5; ++k) {
        VstMidiEvent ev{};
        VstEvents evs{};
        ev.type = 1; ev.byteSize = sizeof(VstMidiEvent);
        ev.midiData[0] = (char)0xb0; ev.midiData[1] = (char)kCc; ev.midiData[2] = (char)vals[k];
        evs.numEvents = 1; evs.events[0] = &ev;
        eff->dispatcher(eff, effProcessEvents, 0, 0, &evs, 0);
        for (int b = 0; b < 4; ++b) eff->processReplacing(eff, in, out, kBlock);
        xs[k] = get(eff, kMorphX); ys[k] = get(eff, kMorphY);
        printf("  CC%d=%3d -> Morph X=%.3f Y=%.3f\n", kCc, vals[k], xs[k], ys[k]);
    }
    eff->dispatcher(eff, effMainsChanged, 0, 0, nullptr, 0);
    bool moved = false;
    for (int k = 1; k < 5; ++k)
        if (xs[k] != xs[0] || ys[k] != ys[0]) moved = true;
    printf("RESULT: %s\n", moved ? "morph follows the CC" : "MORPH DID NOT MOVE");
    return moved ? 0 : 1;
}

int main(int argc, char** argv) {
    if (argc < 2) { printf("usage: vst2probe <plugin.dll> [out.bmp|--arp|--morph] [seconds]\n"); return 2; }
    const char* dllPath = argv[1];
    const bool morphMode = argc > 2 && strcmp(argv[2], "--morph") == 0;
    const bool arpMode = argc > 2 && strcmp(argv[2], "--arp") == 0;
    const char* out = (argc > 2 && !arpMode && !morphMode) ? argv[2] : nullptr;
    const double secs = argc > 3 ? atof(argv[3]) : 3.0;

    printf("probe is %d-bit\n", (int)sizeof(void*) * 8);
    HMODULE m = LoadLibraryA(dllPath);
    if (!m) { printf("FAIL LoadLibrary %s -> %lu\n", dllPath, GetLastError()); return 1; }

    auto entry = (AEffect * (*)(intptr_t(*)(AEffect*, int32_t, int32_t, intptr_t, void*, float)))
                 (void*)GetProcAddress(m, "VSTPluginMain");
    if (!entry) { printf("FAIL no VSTPluginMain\n"); return 1; }

    AEffect* eff = entry(&VSTCALLBACK_host);
    if (!eff || eff->magic != 0x56737450 /*'VstP'*/) { printf("FAIL bad AEffect\n"); return 1; }
    printf("loaded: uniqueID=0x%08x numParams=%d flags=0x%x\n", eff->uniqueID, eff->numParams, eff->flags);

    eff->dispatcher(eff, effOpen, 0, 0, nullptr, 0);

    if (morphMode) {
        const int rc = runMorph(eff);
        eff->dispatcher(eff, effClose, 0, 0, nullptr, 0);
        return rc;
    }

    if (arpMode) {
        const int rc = runArp(eff);
        eff->dispatcher(eff, effClose, 0, 0, nullptr, 0);
        return rc;
    }

    ERect* er = nullptr;
    eff->dispatcher(eff, effEditGetRect, 0, 0, &er, 0);
    if (!er) { printf("FAIL no editor rect\n"); return 1; }
    const int ew = er->right - er->left, eh = er->bottom - er->top;
    printf("effEditGetRect -> %dx%d\n", ew, eh);

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"FM8PlusProbe";
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClassExW(&wc);
    RECT want{0, 0, ew, eh};
    AdjustWindowRect(&want, WS_OVERLAPPEDWINDOW, FALSE);
    HWND host = CreateWindowExW(0, wc.lpszClassName, L"FM8.plus probe", WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT, want.right - want.left,
                                want.bottom - want.top, nullptr, nullptr, wc.hInstance, nullptr);
    ShowWindow(host, SW_SHOW);

    const intptr_t opened = eff->dispatcher(eff, effEditOpen, 0, 0, host, 0);
    printf("effEditOpen -> %d\n", (int)opened);
    pump(secs);

    HWND child = GetWindow(host, GW_CHILD);
    int cw = 0, ch = 0;
    if (child) { RECT cr{}; GetClientRect(child, &cr); cw = cr.right; ch = cr.bottom; }
    wchar_t cls[64] = {};
    if (child) GetClassNameW(child, cls, 63);
    printf("editor child: %ls %dx%d\n", child ? cls : L"(none)", cw, ch);

    if (out && dump(host, out)) printf("screenshot -> %s\n", out);

    const bool ok = opened && child && cw == ew && ch == eh;
    printf("RESULT: %s (rect %dx%d, child %dx%d)\n", ok ? "editor opened at the reported size"
                                                        : "MISMATCH", ew, eh, cw, ch);
    eff->dispatcher(eff, effEditClose, 0, 0, nullptr, 0);
    eff->dispatcher(eff, effClose, 0, 0, nullptr, 0);
    return ok ? 0 : 1;
}
