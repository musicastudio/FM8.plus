// Headless DXi host, for testing FM8.plus's DXi without a DAW. It builds the graph a DXi host
// builds (silence source -> synth -> renderer) with the DirectShow filter graph manager, drives the
// synth through the MFX interfaces the way a sequencer does, and checks what comes out.
//
//     dxihost <FM8.plus.dll>                      run the checks (exit code 0 when all pass)
//     dxihost <FM8.plus.dll> --view [--ole]       open the editor the way SONAR does, with live audio
//                                                 and MIDI in, to look at it (--ole: the stock OLE
//                                                 property frame instead of a SONAR-style one)
//     dxihost <FM8.plus.dll> --view --shot out.bmp [--after secs]
//                                                 same, play a chord, dump the window tree and a
//                                                 screenshot of the harness window, then exit
//
// The DLL is loaded directly through DllGetClassObject, so nothing has to be registered. Beside it
// must sit FM8 1.4.1's 32-bit FM8.dll, as it does once installed.
#include <streams.h>
#include <mmreg.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>
#include "../src/dxi/mfx.h"

namespace {

const GUID CLSID_FM8PlusDxi = {0xe49b1fbe, 0xa4a2, 0x42ee, {0x95, 0x30, 0xd2, 0x15, 0x52, 0xa4, 0xec, 0x1e}};
const GUID CLSID_Source = {0x1d6e2a51, 0x0c3b, 0x4a47, {0x9a, 0x11, 0x5e, 0x70, 0x2b, 0x8e, 0x31, 0x01}};
const GUID CLSID_Sink = {0x1d6e2a51, 0x0c3b, 0x4a47, {0x9a, 0x11, 0x5e, 0x70, 0x2b, 0x8e, 0x31, 0x02}};

constexpr int kRate = 44100, kCh = 2, kFrames = 512;
constexpr int kPpq = 960, kBpm = 120;

int g_fails = 0;
void check(bool ok, const char* what) {
    printf("  [%s] %s\n", ok ? " ok " : "FAIL", what);
    if (!ok) ++g_fails;
}

// 32-bit float, as SONAR runs DX plug-ins, or 16-bit PCM for the DirectSound renderer.
void audioType(CMediaType* mt, bool isFloat = true) {
    WAVEFORMATEX w{};
    w.wFormatTag = isFloat ? WAVE_FORMAT_IEEE_FLOAT : WAVE_FORMAT_PCM;
    w.nChannels = kCh;
    w.nSamplesPerSec = kRate;
    w.wBitsPerSample = isFloat ? 32 : 16;
    w.nBlockAlign = (WORD)(kCh * w.wBitsPerSample / 8);
    w.nAvgBytesPerSec = kRate * w.nBlockAlign;
    mt->InitMediaType();
    mt->SetType(&MEDIATYPE_Audio);
    mt->SetSubtype(isFloat ? &MEDIASUBTYPE_IEEE_FLOAT : &MEDIASUBTYPE_PCM);
    mt->SetFormatType(&FORMAT_WaveFormatEx);
    mt->SetFormat((BYTE*)&w, sizeof w);
}

// The host's silence: a DXi synth is clocked by the buffers pushed through its input.
class SilencePin : public CSourceStream {
public:
    SilencePin(HRESULT* phr, CSource* f, long buffers, bool isFloat)
        : CSourceStream(NAME("silence"), phr, f, L"Out"), m_left(buffers), m_bytes(isFloat ? 4 : 2), m_float(isFloat) {}
    HRESULT GetMediaType(CMediaType* mt) override { audioType(mt, m_float); return S_OK; }
    HRESULT DecideBufferSize(IMemAllocator* a, ALLOCATOR_PROPERTIES* p) override {
        p->cBuffers = 2;
        p->cbBuffer = kFrames * kCh * m_bytes;
        ALLOCATOR_PROPERTIES got{};
        return a->SetProperties(p, &got);
    }
    HRESULT FillBuffer(IMediaSample* ms) override {
        if (m_left-- <= 0) return S_FALSE;
        BYTE* p = nullptr;
        ms->GetPointer(&p);
        std::memset(p, 0, kFrames * kCh * m_bytes);
        ms->SetActualDataLength(kFrames * kCh * m_bytes);
        REFERENCE_TIME t0 = m_pos * UNITS / kRate, t1 = (m_pos + kFrames) * UNITS / kRate;
        ms->SetTime(&t0, &t1);
        ms->SetSyncPoint(TRUE);
        ms->SetDiscontinuity(m_pos == 0);
        m_pos += kFrames;
        return S_OK;
    }
private:
    long m_left;
    int m_bytes;
    bool m_float;
    LONGLONG m_pos = 0;
};

class Silence : public CSource {
public:
    Silence(HRESULT* phr, long buffers, bool isFloat = true) : CSource(NAME("silence"), nullptr, CLSID_Source) {
        new SilencePin(phr, this, buffers, isFloat);
    }
};

// Keeps every rendered frame.
class Capture : public CBaseRenderer {
public:
    Capture(HRESULT* phr) : CBaseRenderer(CLSID_Sink, NAME("capture"), nullptr, phr) {}
    HRESULT CheckMediaType(const CMediaType* mt) override { return *mt->Type() == MEDIATYPE_Audio ? S_OK : E_FAIL; }
    HRESULT DoRenderSample(IMediaSample* ms) override {
        BYTE* p = nullptr;
        ms->GetPointer(&p);
        auto* f = (const float*)p;
        std::lock_guard<std::mutex> g(m);
        data.insert(data.end(), f, f + ms->GetActualDataLength() / 4);
        return S_OK;
    }
    std::mutex m;
    std::vector<float> data;
};

// The sequencer's side of MFX: a fixed tempo map and a time converter at kBpm and kPpq.
class Context : public mfx::ITempoMap, public mfx::ITimeConverter {
public:
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == mfx::IID_IMfxTempoMap) *ppv = (mfx::ITempoMap*)this;
        else if (riid == mfx::IID_IMfxTimeConverter) *ppv = (mfx::ITimeConverter*)this;
        else { *ppv = nullptr; return E_NOINTERFACE; }
        AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++refs; }
    STDMETHODIMP_(ULONG) Release() override { return --refs; }
    LONG STDMETHODCALLTYPE TicksToMsecs(LONG t) override { return (LONG)((LONGLONG)t * 60000 / (kBpm * kPpq)); }
    LONG STDMETHODCALLTYPE MsecsToTicks(LONG ms) override { return (LONG)((LONGLONG)ms * kBpm * kPpq / 60000); }
    int STDMETHODCALLTYPE GetTicksPerQuarterNote() override { return kPpq; }
    int STDMETHODCALLTYPE GetTempoIndexForTime(LONG) override { return 0; }
    int STDMETHODCALLTYPE GetTempoCount() override { return 1; }
    HRESULT STDMETHODCALLTYPE GetTempoAt(int, LONG* t, int* bpm100) override { *t = 0; *bpm100 = kBpm * 100; return S_OK; }
    HRESULT STDMETHODCALLTYPE ConvertMfxTime(mfx::Time* t, mfx::TimeFormat to) override {
        if (t->format != mfx::TF_TICKS || to != mfx::TF_SAMPLES) return E_NOTIMPL;
        t->samples = (LONGLONG)t->ticks * kRate * 60 / (kBpm * kPpq);
        t->format = to;
        return S_OK;
    }
    ULONG refs = 1;
};

template <class I, class T> class Queue : public I {
public:
    STDMETHODIMP QueryInterface(REFIID, void** ppv) override { *ppv = nullptr; return E_NOINTERFACE; }
    STDMETHODIMP_(ULONG) AddRef() override { return 2; }
    STDMETHODIMP_(ULONG) Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE Add(const T& e) override { items.push_back(e); return S_OK; }
    HRESULT STDMETHODCALLTYPE GetCount(int* n) override { *n = (int)items.size(); return S_OK; }
    HRESULT STDMETHODCALLTYPE GetAt(int i, T* e) override { *e = items[i]; return S_OK; }
    std::vector<T> items;
};

// Push `buffers` of silence through the synth and return what came out.
std::vector<float> play(IUnknown* synth, long buffers) {
    HRESULT hr = S_OK;
    IGraphBuilder* g = nullptr;
    CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER, IID_IGraphBuilder, (void**)&g);
    auto* src = new Silence(&hr, buffers);
    auto* cap = new Capture(&hr);
    src->AddRef();
    cap->AddRef();
    IBaseFilter* dxi = nullptr;
    synth->QueryInterface(IID_IBaseFilter, (void**)&dxi);
    g->AddFilter(src, L"silence");
    g->AddFilter(dxi, L"FM8.plus");
    g->AddFilter(cap, L"capture");
    CMediaType mt;
    audioType(&mt);
    IPin *srcOut = src->GetPin(0), *dxiIn = nullptr, *dxiOut = nullptr, *capIn = cap->GetPin(0);
    dxi->FindPin(L"Input", &dxiIn);
    dxi->FindPin(L"Output", &dxiOut);
    const HRESULT c1 = g->ConnectDirect(srcOut, dxiIn, &mt), c2 = g->ConnectDirect(dxiOut, capIn, nullptr);
    check(SUCCEEDED(c1) && SUCCEEDED(c2), "graph connects: silence -> FM8.plus -> capture");
    IMediaFilter* mf = nullptr;
    g->QueryInterface(IID_IMediaFilter, (void**)&mf);
    mf->SetSyncSource(nullptr);   // no clock: render as fast as the synth can
    IMediaControl* mc = nullptr;
    IMediaEvent* me = nullptr;
    g->QueryInterface(IID_IMediaControl, (void**)&mc);
    g->QueryInterface(IID_IMediaEvent, (void**)&me);
    mc->Run();
    long code = 0;
    me->WaitForCompletion(20000, &code);
    mc->Stop();
    std::vector<float> out;
    { std::lock_guard<std::mutex> lk(cap->m); out = cap->data; }
    for (IUnknown* u : {(IUnknown*)dxiIn, (IUnknown*)dxiOut, (IUnknown*)mf, (IUnknown*)mc, (IUnknown*)me}) if (u) u->Release();
    g->RemoveFilter(src);
    g->RemoveFilter(dxi);
    g->RemoveFilter(cap);
    dxi->Release();
    src->Release();
    cap->Release();
    g->Release();
    return out;
}

float peak(const std::vector<float>& v, size_t from = 0, size_t to = SIZE_MAX) {
    float p = 0;
    for (size_t i = from; i < v.size() && i < to; ++i) p = std::max(p, std::fabs(v[i]));
    return p;
}

using GetClassObject = HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, void**);

// --- viewer --------------------------------------------------------------------------------------

// A live graph like a DAW engine's: silence -> synth -> DirectSound, clocked by the sound card, so
// FM8 runs and animates in real time while its editor is up. Without DirectSound the synth still
// runs, into a clocked null renderer.
struct Live {
    IGraphBuilder* g = nullptr;
    IMediaControl* mc = nullptr;
    bool start(IUnknown* synth) {
        HRESULT hr = S_OK;
        CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER, IID_IGraphBuilder, (void**)&g);
        auto* src = new Silence(&hr, LONG_MAX, false);
        IBaseFilter *dxi = nullptr, *out = nullptr;
        synth->QueryInterface(IID_IBaseFilter, (void**)&dxi);
        if (FAILED(CoCreateInstance(CLSID_DSoundRender, nullptr, CLSCTX_INPROC_SERVER, IID_IBaseFilter, (void**)&out)))
            out = new Capture(&hr), out->AddRef();
        g->AddFilter(src, L"silence");
        g->AddFilter(dxi, L"FM8.plus");
        g->AddFilter(out, L"output");
        CMediaType mt;
        audioType(&mt, false);
        IPin *dxiIn = nullptr, *dxiOut = nullptr, *outIn = nullptr;
        IEnumPins* e = nullptr;
        dxi->FindPin(L"Input", &dxiIn);
        dxi->FindPin(L"Output", &dxiOut);
        out->EnumPins(&e);
        e->Next(1, &outIn, nullptr);
        e->Release();
        const HRESULT c1 = g->ConnectDirect(src->GetPin(0), dxiIn, &mt), c2 = g->ConnectDirect(dxiOut, outIn, nullptr);
        for (IUnknown* u : {(IUnknown*)dxiIn, (IUnknown*)dxiOut, (IUnknown*)outIn, (IUnknown*)dxi, (IUnknown*)out}) u->Release();
        g->QueryInterface(IID_IMediaControl, (void**)&mc);
        const HRESULT run = mc->Run();
        printf("live graph: silence -> FM8.plus 0x%08lx, FM8.plus -> output 0x%08lx, run 0x%08lx\n", c1, c2, run);
        return SUCCEEDED(c1) && SUCCEEDED(c2) && SUCCEEDED(run);
    }
    void stop() {
        if (mc) mc->Stop(), mc->Release();
        if (g) g->Release();   // the graph releases the filters it holds
    }
};

// The host's side of a property page. SONAR's DX window is a frame of its own around the page.
class Site : public IPropertyPageSite {
public:
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        *ppv = riid == IID_IUnknown || riid == IID_IPropertyPageSite ? this : nullptr;
        return *ppv ? S_OK : E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return 2; }
    STDMETHODIMP_(ULONG) Release() override { return 1; }
    STDMETHODIMP OnStatusChange(DWORD) override { return S_OK; }
    STDMETHODIMP GetLocaleID(LCID* l) override { *l = GetUserDefaultLCID(); return S_OK; }
    STDMETHODIMP GetPageContainer(IUnknown** u) override { *u = nullptr; return E_NOTIMPL; }
    STDMETHODIMP TranslateAccelerator(MSG*) override { return S_FALSE; }
};

// SONAR's DX window, as its window tree shows it: a top-level frame, and inside it a host area that
// does NOT clip its children, holding the Presets strip and the page at (5, 31).
constexpr int kBar = 31, kPad = 5;
const BYTE kChordKeys[] = {60, 64, 67};
enum { kChord = 1, kReopen, kShot, kRepaint, kNotesOff = 10, kAutoChord, kAutoShot, kAutoRepaint, kAutoGrow, kAutoStrips };

struct View {
    mfx::ISoftSynth2* ss = nullptr;
    IPropertyPage* page = nullptr;
    SIZE size{};
    const char* shot = nullptr;
    bool drag = false;
    HWND frame = nullptr, host = nullptr;
} g_view;

void live(BYTE s, BYTE d1, BYTE d2) {
    Queue<mfx::IDataQueue, mfx::Data> q;
    q.items.push_back(mfx::Data{0, s, d1, d2, 0});
    g_view.ss->OnInput(0, &q);
}

void chord(HWND timerWnd) {
    for (BYTE k : kChordKeys) live(0x90, k, 100);
    SetTimer(timerWnd, kNotesOff, 1200, nullptr);
}

void CALLBACK midiIn(HMIDIIN, UINT msg, DWORD_PTR, DWORD_PTR p1, DWORD_PTR) {
    if (msg == MIM_DATA && (p1 & 0xf0) >= 0x80 && (p1 & 0xf0) < 0xf0) live((BYTE)p1, (BYTE)(p1 >> 8), (BYTE)(p1 >> 16));
}

// Every window under `root`, as laid out in root's client area: what the host and FM8 created.
void dumpTree(HWND root) {
    RECT rc;
    GetClientRect(root, &rc);
    printf("window tree, client %ldx%ld:\n", rc.right, rc.bottom);
    EnumChildWindows(root, [](HWND h, LPARAM r) -> BOOL {
        int depth = 0;
        for (HWND p = GetParent(h); p && p != (HWND)r; p = GetParent(p)) ++depth;
        char cls[64];
        GetClassNameA(h, cls, sizeof cls);
        RECT w;
        GetWindowRect(h, &w);
        MapWindowPoints(nullptr, (HWND)r, (POINT*)&w, 2);
        const LONG st = GetWindowLongW(h, GWL_STYLE), ex = GetWindowLongW(h, GWL_EXSTYLE);
        printf("  %*s%-28s %5ld,%-5ld %4ldx%-4ld%s%s%s ex=%08lx\n", depth * 2, "", cls, w.left, w.top, w.right - w.left,
               w.bottom - w.top, st & WS_VISIBLE ? " visible" : " HIDDEN", st & WS_CLIPCHILDREN ? " clipchildren" : "",
               st & WS_CLIPSIBLINGS ? " clipsiblings" : "", ex);
        return TRUE;
    }, (LPARAM)root);
}

// The harness window's client area (its own window only) as a 32-bit BMP.
void screenshot(HWND h, const char* path) {
    RECT rc;
    GetClientRect(h, &rc);
    const int w = rc.right, ht = rc.bottom;
    HDC dc = GetDC(h), mem = CreateCompatibleDC(dc);
    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), w, -ht, 1, 32, BI_RGB};
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(mem, bmp);
    // Copy what is actually on the window (its composed pixels), not a fresh WM_PRINT repaint, which
    // would hide exactly the overdraw a host can leave behind.
    BitBlt(mem, 0, 0, w, ht, dc, 0, 0, SRCCOPY);
    const DWORD size = (DWORD)w * 4 * ht;
    BITMAPFILEHEADER fh{0x4d42, (DWORD)(sizeof fh + sizeof bi.bmiHeader + size), 0, 0, sizeof fh + sizeof bi.bmiHeader};
    if (FILE* f = fopen(path, "wb")) {
        fwrite(&fh, sizeof fh, 1, f);
        fwrite(&bi.bmiHeader, sizeof bi.bmiHeader, 1, f);
        fwrite(bits, size, 1, f);
        fclose(f);
        printf("screenshot %dx%d -> %s\n", w, ht, path);
    }
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(h, dc);
}

// What SONAR's DX window does after the editor is up: its frame and host area repaint themselves
// alone. They do not clip their children, so unless the page protects itself this paints over FM8's
// editor, and FM8 only redraws what it animates.
void hostRepaint() {
    for (HWND w : {g_view.frame, g_view.host})
        RedrawWindow(w, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_NOCHILDREN | RDW_UPDATENOW);
}

// What happened in SONAR after a GUI Scale change: the editor grew but the DX window did not, and the
// user dragged the window bigger, which sends the editor one partial repaint after another. Start
// the frame at half the editor's size and grow it the same way, a step at a time.
void dragToFit() {
    RECT full{0, 0, g_view.size.cx + 2 * kPad, g_view.size.cy + kBar + kPad};
    AdjustWindowRect(&full, WS_OVERLAPPEDWINDOW, FALSE);
    const int fw = full.right - full.left, fh = full.bottom - full.top;
    // One edge at a time, as a user drags: each step then exposes a strip away from the origin.
    for (int i = 0; i <= 48; ++i) {
        const int w = fw / 2 + (fw - fw / 2) * std::min(i, 24) / 24, h = fh / 2 + (fh - fh / 2) * std::max(i - 24, 0) / 24;
        SetWindowPos(g_view.frame, nullptr, 0, 0, w, h, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        for (DWORD t0 = GetTickCount(); GetTickCount() - t0 < 25;) {
            MSG m;
            while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
            Sleep(5);
        }
    }
}

// The repaints SONAR sent FM8 while its window was dragged bigger (dxi.log): strips that do not start
// at the origin, erased first. Ask FM8's editor for a row and a column band like that.
void stripRepaint() {
    HWND page = FindWindowExW(g_view.host, nullptr, L"FM8plusDxiPage", nullptr);
    HWND editor = page ? GetWindow(page, GW_CHILD) : nullptr;
    if (!editor) return;
    RECT c;
    GetClientRect(editor, &c);
    RECT row{0, c.bottom * 64 / 100, c.right, c.bottom * 64 / 100 + 40}, col{c.right * 63 / 100, 0, c.right * 63 / 100 + 40, c.bottom};
    InvalidateRect(editor, &row, TRUE);
    InvalidateRect(editor, &col, TRUE);
    UpdateWindow(editor);
}

void openPage() {
    RECT rc{kPad, kBar, kPad + g_view.size.cx, kBar + g_view.size.cy};
    printf("Activate at %ld,%ld %ldx%ld: 0x%08lx\n", rc.left, rc.top, g_view.size.cx, g_view.size.cy,
           g_view.page->Activate(g_view.host, &rc, FALSE));
    g_view.page->Show(SW_SHOW);
}

LRESULT CALLBACK frameProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_COMMAND:
            if (LOWORD(wp) == kRepaint) hostRepaint();
            if (LOWORD(wp) == kChord) chord(h);
            if (LOWORD(wp) == kReopen) { g_view.page->Deactivate(); openPage(); }
            if (LOWORD(wp) == kShot) { dumpTree(g_view.frame); screenshot(g_view.frame, "dxiview.bmp"); }
            return 0;
        case WM_TIMER:
            KillTimer(h, wp);
            if (wp == kNotesOff) for (BYTE k : kChordKeys) live(0x80, k, 0);
            if (wp == kAutoChord) chord(h);
            if (wp == kAutoRepaint) hostRepaint();
            if (wp == kAutoGrow) dragToFit();
            if (wp == kAutoStrips) stripRepaint();
            if (wp == kAutoShot) { dumpTree(h); screenshot(h, g_view.shot); PostMessageW(h, WM_CLOSE, 0, 0); }
            return 0;
        case WM_SIZE:
            // Like SONAR's host area: only the newly exposed strips get invalidated, not the lot.
            if (h == g_view.frame && g_view.host)
                SetWindowPos(g_view.host, nullptr, 0, 0, LOWORD(lp), HIWORD(lp), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        case WM_CLOSE:
            g_view.page->Deactivate();
            DestroyWindow(h);
            return 0;
        case WM_DESTROY:
            if (h == g_view.frame) PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// The stock OLE property frame (OleCreatePropertyFrame), which other DX hosts use. It creates the
// page by CLSID, so the page's class object is registered in this process only, never in the registry.
void viewOle(GetClassObject gco, IUnknown* synth, const CLSID& pageId, int after) {
    IClassFactory* cf = nullptr;
    DWORD cookie = 0;
    gco(pageId, IID_IClassFactory, (void**)&cf);
    CoRegisterClassObject(pageId, cf, CLSCTX_INPROC_SERVER, REGCLS_MULTIPLEUSE, &cookie);
    HWND owner = CreateWindowExW(0, L"STATIC", L"owner", 0, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr);
    SetWindowLongPtrW(owner, GWLP_WNDPROC, (LONG_PTR)(WNDPROC)[](HWND h, UINT msg, WPARAM wp, LPARAM lp) -> LRESULT {
        if (msg != WM_TIMER) return DefWindowProcW(h, msg, wp, lp);
        KillTimer(h, wp);
        HWND sheet = nullptr;
        EnumThreadWindows(GetCurrentThreadId(), [](HWND w, LPARAM o) -> BOOL {
            if (GetWindow(w, GW_OWNER) == *(HWND*)o) { *(HWND*)o = w; return FALSE; }
            return TRUE;
        }, (LPARAM)&(sheet = h));
        if (wp == kNotesOff) for (BYTE k : kChordKeys) live(0x80, k, 0);
        if (wp == kAutoChord) chord(h);
        if (wp == kAutoShot && sheet != h) { dumpTree(sheet); screenshot(sheet, g_view.shot); PostMessageW(sheet, WM_COMMAND, IDCANCEL, 0); }
        return 0;
    });
    if (g_view.shot) { SetTimer(owner, kAutoChord, 300, nullptr); SetTimer(owner, kAutoShot, after * 1000, nullptr); }
    IUnknown* objs[] = {synth};
    CLSID pages[] = {pageId};
    printf("OleCreatePropertyFrame: 0x%08lx\n", OleCreatePropertyFrame(owner, 40, 40, L"FM8.plus", 1, objs, 1, pages, 0, 0, nullptr));
    DestroyWindow(owner);
    CoRevokeClassObject(cookie);
    cf->Release();
}

int view(GetClassObject gco, IUnknown* synth, mfx::ISoftSynth2* ss, bool ole, const char* shot, int after) {
    g_view.ss = ss;
    g_view.shot = shot;
    Live engine;
    engine.start(synth);
    std::vector<HMIDIIN> ins;
    for (UINT i = 0; i < midiInGetNumDevs(); ++i) {
        HMIDIIN in = nullptr;
        MIDIINCAPSA caps{};
        midiInGetDevCapsA(i, &caps, sizeof caps);
        if (midiInOpen(&in, i, (DWORD_PTR)&midiIn, 0, CALLBACK_FUNCTION) == MMSYSERR_NOERROR) {
            midiInStart(in);
            ins.push_back(in);
            printf("MIDI in: %s\n", caps.szPname);
        }
    }
    ISpecifyPropertyPages* spp = nullptr;
    CAUUID pages{};
    synth->QueryInterface(IID_ISpecifyPropertyPages, (void**)&spp);
    spp->GetPages(&pages);
    const CLSID pageId = pages.pElems[0];
    CoTaskMemFree(pages.pElems);
    spp->Release();

    if (ole) {
        viewOle(gco, synth, pageId, after);
    } else {
        IClassFactory* cf = nullptr;
        gco(pageId, IID_IClassFactory, (void**)&cf);
        cf->CreateInstance(nullptr, IID_IPropertyPage, (void**)&g_view.page);
        cf->Release();
        static Site site;
        IUnknown* objs[] = {synth};
        PROPPAGEINFO pi{};
        g_view.page->SetPageSite(&site);
        g_view.page->SetObjects(1, objs);
        g_view.page->GetPageInfo(&pi);
        g_view.size = pi.size;
        printf("page \"%ls\" %ldx%ld\n", pi.pszTitle, pi.size.cx, pi.size.cy);
        CoTaskMemFree(pi.pszTitle);

        WNDCLASSW wc{};
        wc.lpfnWndProc = &frameProc;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"FM8plusDxiFrame";
        RegisterClassW(&wc);
        RECT rc{0, 0, pi.size.cx + 2 * kPad, pi.size.cy + kBar + kPad};
        AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
        if (g_view.drag) rc.right = rc.left + (rc.right - rc.left) / 2, rc.bottom = rc.top + (rc.bottom - rc.top) / 2;
        g_view.frame = CreateWindowExW(0, wc.lpszClassName, L"FM8.plus (DXi) - SONAR-style frame", WS_OVERLAPPEDWINDOW | WS_CLIPSIBLINGS,
                                       60, 60, rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, nullptr, nullptr);
        GetClientRect(g_view.frame, &rc);
        g_view.host = CreateWindowExW(0, wc.lpszClassName, L"", WS_CHILD | WS_VISIBLE, 0, 0, rc.right, rc.bottom, g_view.frame,
                                      nullptr, nullptr, nullptr);
        int x = kPad;
        for (auto [id, text] : {std::pair{kChord, L"Chord"}, std::pair{kReopen, L"Reopen editor"}, std::pair{kShot, L"Screenshot"}, std::pair{kRepaint, L"Host repaint"}}) {
            CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, x, 3, 100, kBar - 8, g_view.host,
                            (HMENU)(INT_PTR)id, nullptr, nullptr);
            x += 104;
        }
        // How SONAR opens a DX page (its log, dxi.log): the frame is up, but with redraw switched off,
        // which clears its visible bit, while the page is activated and shown. Redraw then comes back
        // on and the frame repaints itself, without invalidating the page inside it.
        ShowWindow(g_view.frame, SW_SHOW);
        UpdateWindow(g_view.frame);
        SendMessageW(g_view.frame, WM_SETREDRAW, FALSE, 0);
        openPage();
        g_view.page->Show(SW_SHOW);
        SendMessageW(g_view.frame, WM_SETREDRAW, TRUE, 0);
        RedrawWindow(g_view.frame, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_NOCHILDREN | RDW_UPDATENOW);
        if (shot) {
            SetTimer(g_view.frame, kAutoChord, 300, nullptr);
            SetTimer(g_view.frame, kAutoRepaint, 1500, nullptr);
            if (g_view.drag) SetTimer(g_view.frame, kAutoGrow, 800, nullptr);
            SetTimer(g_view.frame, kAutoStrips, 2200, nullptr);
            SetTimer(g_view.frame, kAutoShot, after * 1000, nullptr);
        }
        MSG m;
        while (GetMessageW(&m, nullptr, 0, 0) > 0) {
            if (g_view.page->TranslateAccelerator(&m) == S_OK) continue;
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        g_view.page->SetObjects(0, nullptr);
        g_view.page->Release();
    }
    for (HMIDIIN in : ins) midiInStop(in), midiInClose(in);
    engine.stop();
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) { printf("usage: dxihost <FM8.plus.dll> [--view [--ole] [--shot out.bmp [--after secs]]]\n"); return 2; }
    bool viewMode = false, ole = false;
    const char* shot = nullptr;
    int after = 4;
    for (int i = 2; i < argc; ++i) {
        if (!strcmp(argv[i], "--view")) viewMode = true;
        else if (!strcmp(argv[i], "--ole")) ole = true;
        else if (!strcmp(argv[i], "--drag")) g_view.drag = true;
        else if (!strcmp(argv[i], "--shot") && i + 1 < argc) shot = argv[++i];
        else if (!strcmp(argv[i], "--after") && i + 1 < argc) after = atoi(argv[++i]);
    }
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    HMODULE dll = LoadLibraryA(argv[1]);
    if (!dll) { printf("cannot load %s (%lu)\n", argv[1], GetLastError()); return 1; }
    auto gco = (GetClassObject)GetProcAddress(dll, "DllGetClassObject");
    check(gco && GetProcAddress(dll, "DllRegisterServer") && GetProcAddress(dll, "VSTPluginMain"), "exports DllGetClassObject, DllRegisterServer and VSTPluginMain");
    if (!gco) return 1;

    IClassFactory* cf = nullptr;
    IUnknown* synth = nullptr;
    gco(CLSID_FM8PlusDxi, IID_IClassFactory, (void**)&cf);
    if (cf) cf->CreateInstance(nullptr, IID_IUnknown, (void**)&synth);
    check(synth != nullptr, "creates the FM8.plus DXi (FM8 1.4.1 loaded behind it)");
    if (!synth) return 1;

    mfx::ISoftSynth2* ss = nullptr;
    synth->QueryInterface(mfx::IID_IMfxSoftSynth2, (void**)&ss);
    IBaseFilter* bf = nullptr;
    synth->QueryInterface(IID_IBaseFilter, (void**)&bf);
    IPersistStream* ps = nullptr;
    synth->QueryInterface(IID_IPersistStream, (void**)&ps);
    ISpecifyPropertyPages* spp = nullptr;
    synth->QueryInterface(IID_ISpecifyPropertyPages, (void**)&spp);
    check(ss && bf && ps && spp, "answers IMfxSoftSynth2, IBaseFilter, IPersistStream, ISpecifyPropertyPages");

    Context ctx;
    ctx.AddRef();   // MFX: the synth releases the context it is handed
    check(SUCCEEDED(ss->Connect((mfx::ITempoMap*)&ctx)) && ctx.refs >= 1, "IMfxSoftSynth::Connect takes the tempo map");
    if (viewMode) {
        view(gco, synth, ss, ole, shot, after);
        ss->Disconnect();
        for (IUnknown* u : {(IUnknown*)spp, (IUnknown*)ps, (IUnknown*)bf, (IUnknown*)ss, (IUnknown*)cf, synth}) if (u) u->Release();
        CoUninitialize();
        return 0;
    }

    const long bufs = kRate / kFrames;   // one second
    const size_t quarter = (size_t)kRate * 60 / kBpm * kCh;   // a quarter note of interleaved samples

    printf("-- playback: one quarter note at tick 0\n");
    Queue<mfx::IEventQueue, mfx::Event> q;
    mfx::Event e{};
    e.time = 0;
    e.type = mfx::Event::Note;
    e.u.note.key = 60;
    e.u.note.vel = 110;
    e.u.note.velOff = 64;
    e.u.note.duration = kPpq;
    q.items.push_back(e);
    ss->OnStart(0);
    ss->OnEvents(0, 4 * kPpq, 0, &q);
    auto out = play(synth, bufs);
    ss->OnStop((LONG)((LONGLONG)bufs * kFrames * kBpm * kPpq / (60 * kRate)));
    printf("     rendered %zu frames, peak %.4f during the note, %.4f in the last 50 ms\n",
           out.size() / kCh, peak(out, 0, quarter), peak(out, out.size() - (size_t)kRate / 20 * kCh));
    check(out.size() == (size_t)bufs * kFrames * kCh, "renders exactly the frames pushed in");
    check(peak(out, 0, quarter) > 0.001f, "the note sounds");

    printf("-- no events\n");
    ss->OnStart(0);
    out = play(synth, bufs / 4);
    ss->OnStop(0);
    printf("     peak %.6f\n", peak(out));
    check(peak(out) < 0.0001f, "silence with no notes");

    printf("-- live input: note-on through OnInput\n");
    Queue<mfx::IDataQueue, mfx::Data> live;
    live.items.push_back(mfx::Data{0, 0x90, 64, 100, 0});
    ss->OnInput(0, &live);
    out = play(synth, bufs / 4);
    printf("     peak %.4f\n", peak(out));
    check(peak(out) > 0.001f, "a live note sounds");
    live.items.assign(1, mfx::Data{0, 0x80, 64, 0, 0});
    ss->OnInput(0, &live);

    printf("-- state\n");
    IStream* st = nullptr;
    CreateStreamOnHGlobal(nullptr, TRUE, &st);
    const HRESULT sv = ps->Save(st, TRUE);
    STATSTG ss2{};
    st->Stat(&ss2, STATFLAG_NONAME);
    std::vector<char> blob((size_t)ss2.cbSize.QuadPart);
    LARGE_INTEGER zero{};
    st->Seek(zero, STREAM_SEEK_SET, nullptr);
    st->Read(blob.data(), (ULONG)blob.size(), nullptr);
    const bool trailer = std::search(blob.begin(), blob.end(), "FM8PLUS2", "FM8PLUS2" + 8) != blob.end();
    printf("     saved %zu bytes\n", blob.size());
    check(SUCCEEDED(sv) && blob.size() > 1000 && trailer, "IPersistStream saves FM8's chunk with the FM8.plus settings");
    st->Seek(zero, STREAM_SEEK_SET, nullptr);
    check(SUCCEEDED(ps->Load(st)), "IPersistStream loads it back");
    st->Release();

    printf("-- editor page\n");
    CAUUID pages{};
    spp->GetPages(&pages);
    IClassFactory* pcf = nullptr;
    IPropertyPage* page = nullptr;
    if (pages.cElems) gco(pages.pElems[0], IID_IClassFactory, (void**)&pcf);
    if (pcf) pcf->CreateInstance(nullptr, IID_IPropertyPage, (void**)&page);
    CoTaskMemFree(pages.pElems);
    check(page != nullptr, "ISpecifyPropertyPages names a page this DLL creates");
    if (page) {
        IUnknown* objs[] = {synth};
        PROPPAGEINFO pi{};
        const HRESULT so = page->SetObjects(1, objs);
        page->GetPageInfo(&pi);
        printf("     page \"%ls\" %ldx%ld\n", pi.pszTitle ? pi.pszTitle : L"?", pi.size.cx, pi.size.cy);
        CoTaskMemFree(pi.pszTitle);
        HWND frame = CreateWindowExW(0, L"STATIC", L"frame", WS_OVERLAPPEDWINDOW, 0, 0, pi.size.cx + 40, pi.size.cy + 60,
                                     nullptr, nullptr, nullptr, nullptr);
        RECT rc{0, 0, pi.size.cx, pi.size.cy};
        const HRESULT ac = page->Activate(frame, &rc, FALSE);
        page->Show(SW_SHOW);
        for (DWORD t0 = GetTickCount(); GetTickCount() - t0 < 1500;) {
            MSG m;
            while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
            Sleep(10);
        }
        HWND pageWnd = FindWindowExW(frame, nullptr, L"FM8plusDxiPage", nullptr);
        const bool editor = pageWnd && GetWindow(pageWnd, GW_CHILD);
        check(SUCCEEDED(so) && SUCCEEDED(ac) && pi.size.cx > 300 && editor, "the page opens FM8's editor inside it");
        page->Deactivate();
        page->SetObjects(0, nullptr);
        page->Release();
        DestroyWindow(frame);
    }

    ss->Disconnect();
    for (IUnknown* u : {(IUnknown*)spp, (IUnknown*)ps, (IUnknown*)bf, (IUnknown*)ss, (IUnknown*)cf}) if (u) u->Release();
    if (pcf) pcf->Release();
    check(synth->Release() == 0, "the synth frees itself on its last Release");
    printf(g_fails ? "\n%d check(s) failed\n" : "\nall checks passed\n", g_fails);
    CoUninitialize();
    return g_fails ? 1 : 0;
}
