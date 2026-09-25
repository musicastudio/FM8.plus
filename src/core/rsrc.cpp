#include "rsrc.h"
#include <shlwapi.h>
#include <cstdint>
#include <cstring>
#include <vector>
// GDI+ headers reference the min/max macros that the project's NOMINMAX removes; restore them here.
#ifndef min
#define min(a, b) (((a) < (b)) ? (a) : (b))
#define max(a, b) (((a) > (b)) ? (a) : (b))
#define FM8PLUS_TMP_MINMAX
#endif
#pragma warning(push)
#pragma warning(disable : 4458)   // the Windows SDK's own GDI+ headers shadow members under /W4
#include <gdiplus.h>
#pragma warning(pop)
#ifdef FM8PLUS_TMP_MINMAX
#undef min
#undef max
#undef FM8PLUS_TMP_MINMAX
#endif

namespace fm8plus::Rsrc {
namespace {

struct Entry { const char* type; int id; const void* data; size_t size; };
constexpr int kMax = 32;
Entry g_entries[kMax];           // a fake HRSRC/HGLOBAL is a pointer into this array
int g_count = 0;
HMODULE g_fm8 = nullptr;

using FindResourceA_t   = HRSRC   (WINAPI*)(HMODULE, LPCSTR, LPCSTR);
using SizeofResource_t  = DWORD   (WINAPI*)(HMODULE, HRSRC);
using LoadResource_t    = HGLOBAL (WINAPI*)(HMODULE, HRSRC);
using LockResource_t    = LPVOID  (WINAPI*)(HGLOBAL);
FindResourceA_t  o_find = nullptr;
SizeofResource_t o_size = nullptr;
LoadResource_t   o_load = nullptr;
LockResource_t   o_lock = nullptr;

Entry* ours(const void* h) {
    return (h >= g_entries && h < g_entries + g_count) ? (Entry*)h : nullptr;
}

// Serve `data` for resource `type`/`id`. The bytes must outlive the process's use of them.
void serve(const char* type, int id, const void* data, size_t size) {
    for (int i = 0; i < g_count; ++i)
        if (g_entries[i].id == id && _stricmp(type, g_entries[i].type) == 0) {
            g_entries[i] = {type, id, data, size};
            return;
        }
    if (g_count < kMax) g_entries[g_count++] = {type, id, data, size};
}

// Read a resource straight out of the FM8 module, bypassing anything we serve.
const void* stock(const char* type, int id, size_t& size) {
    size = 0;
    if (!g_fm8 || !o_find) return nullptr;
    HRSRC r = o_find(g_fm8, MAKEINTRESOURCEA(id), type);
    if (!r) return nullptr;
    HGLOBAL g = o_load(g_fm8, r);
    if (!g) return nullptr;
    size = o_size(g_fm8, r);
    return o_lock(g);
}

// ---- the "FM8+" wordmark ---------------------------------------------------------------------
// FM8's wordmark is control 5 of FRM 5 and 15, a 95x23 Switch at (21,35) drawn from PICTURE 193. We
// move the control kShift px left (there is only 17px of clear space before the sound-name label)
// and grow it kPlusW px right, and widen the picture to match with the "+" drawn into the new space.
// src/core/ui.cpp repeats these numbers to hit-test the click; tools/logo_preview.py draws the
// same cross to preview a layout change.
constexpr int32_t kLogoX1 = 21, kLogoY1 = 35, kLogoX2 = 116, kLogoY2 = 58;
constexpr int32_t kShift = 11, kPlusW = 22;
constexpr int kTextDx = 4;   // the "FM8" artwork sits this far right in the widened picture
constexpr float kPlusCx = 108.4f, kPlusCy = 12.46f;           // centre, in the widened picture
constexpr float kPlusHalf = 7.37f, kPlusThick = 1.515f;       // arm span and bar thickness, halved
constexpr float kPlusSlant = 0.1767f;                         // tan(10 degrees) italic shear
constexpr uint32_t kPlusRgb = 0x6b7d86;                       // sampled from the wordmark itself

std::vector<uint8_t> g_frm5, g_frm15, g_pic;   // served blobs, alive for the life of the process
bool g_wantPicture = false;

// FRM 5 and 15 carry the wordmark's rect as four little-endian int32s, and nothing else in either
// form has that rect (checked against all 74), so a copy and a single pattern rewrite is the whole
// edit; no FRM parser needed at runtime.
bool widenForm(int id, std::vector<uint8_t>& out) {
    size_t n = 0;
    auto* p = (const uint8_t*)stock("FRM", id, n);
    if (!p || n < 16) return false;
    out.assign(p, p + n);
    const int32_t from[4] = {kLogoX1, kLogoY1, kLogoX2, kLogoY2};
    const int32_t to[4]   = {kLogoX1 - kShift, kLogoY1, kLogoX2 + kPlusW - kShift, kLogoY2};
    for (size_t o = 0; o + sizeof from <= out.size(); ++o)
        if (std::memcmp(out.data() + o, from, sizeof from) == 0) {
            std::memcpy(out.data() + o, to, sizeof to);
            return true;
        }
    out.clear();
    return false;
}

// The "+" is a 12-vertex slanted cross: two 3px bars sheared 10 degrees to sit italic beside FM8's
// own wordmark. Drawn straight into the pixels, with 4x4 coverage sampling for the antialiased
// edges, so FM8.plus needs no font and no artwork of its own to produce it.
void plusOutline(float* px, float* py) {
    const float a = kPlusHalf, t = kPlusThick;
    const float xs[12] = { a,  t,  t, -t, -t, -a, -a, -t, -t,  t,  t,  a};
    const float ys[12] = {-t, -t, -a, -a, -t, -t,  t,  t,  a,  a,  t,  t};
    for (int i = 0; i < 12; ++i) {
        px[i] = kPlusCx + xs[i] - ys[i] * kPlusSlant;
        py[i] = kPlusCy + ys[i];
    }
}

bool inPlus(const float* px, const float* py, float x, float y) {
    bool in = false;
    for (int i = 0, j = 11; i < 12; j = i++)
        if ((py[i] > y) != (py[j] > y) && x < (px[j] - px[i]) * (y - py[i]) / (py[j] - py[i]) + px[i])
            in = !in;
    return in;
}

void drawPlus(uint32_t* px, int w, int h) {   // pixels are 0xAARRGGBB
    float ox[12], oy[12];
    plusOutline(ox, oy);
    const int x0 = (int)(kPlusCx - kPlusHalf - 3), x1 = (int)(kPlusCx + kPlusHalf + 3);
    const int y0 = (int)(kPlusCy - kPlusHalf - 3), y1 = (int)(kPlusCy + kPlusHalf + 3);
    for (int y = y0 < 0 ? 0 : y0; y <= y1 && y < h; ++y)
        for (int x = x0 < 0 ? 0 : x0; x <= x1 && x < w; ++x) {
            int cov = 0;
            for (int sy = 0; sy < 4; ++sy)
                for (int sx = 0; sx < 4; ++sx)
                    if (inPlus(ox, oy, x + (sx + 0.5f) / 4.0f, y + (sy + 0.5f) / 4.0f)) ++cov;
            // ponytail: the cross sits entirely in the new space, which is empty, so a plain write
            // is the composite. Blend here if the geometry ever moves over the wordmark itself.
            if (cov) px[(size_t)y * w + x] = ((uint32_t)(cov * 255 / 16) << 24) | kPlusRgb;
        }
}

// FM8's Picture::Load tries PNG, then JPEG, then TGA (docs/gui-resources.md 2), and an uncompressed
// 32-bit TGA is an 18-byte header in front of exactly the BGRA bytes it wants in memory. So we hand
// it that rather than re-encoding a PNG.
void toTga(const uint32_t* px, int w, int h, std::vector<uint8_t>& out) {
    out.assign(18 + (size_t)w * h * 4, 0);
    out[2] = 2;                                        // uncompressed true colour
    out[12] = (uint8_t)w;  out[13] = (uint8_t)(w >> 8);
    out[14] = (uint8_t)h;  out[15] = (uint8_t)(h >> 8);
    out[16] = 32;                                      // bits per pixel
    out[17] = 0x28;                                    // top-down rows, 8 alpha bits
    uint8_t* p = out.data() + 18;
    for (int i = 0; i < w * h; ++i) {
        const uint32_t c = px[i];
        *p++ = (uint8_t)c; *p++ = (uint8_t)(c >> 8); *p++ = (uint8_t)(c >> 16); *p++ = (uint8_t)(c >> 24);
    }
}

// Decode FM8's own wordmark PNG, widen it, draw the "+", serve it back as a TGA. Deferred to the
// first request (see h_find): this needs GDI+, and the standalone registers everything from DllMain,
// where loading a DLL would take the loader lock.
bool buildPicture() {
    size_t n = 0;
    const void* png = stock("PICTURE", 193, n);
    if (!png || !n) return false;

    ULONG_PTR token = 0;
    Gdiplus::GdiplusStartupInput in;
    if (Gdiplus::GdiplusStartup(&token, &in, nullptr) != Gdiplus::Ok) return false;
    bool ok = false;
    if (IStream* s = SHCreateMemStream((const BYTE*)png, (UINT)n)) {
        {
            Gdiplus::Bitmap bmp(s, FALSE);   // must outlive nothing past this scope: the stream does
            const int w = (int)bmp.GetWidth(), h = (int)bmp.GetHeight();
            Gdiplus::Rect r(0, 0, w, h);
            Gdiplus::BitmapData bd;
            if (bmp.GetLastStatus() == Gdiplus::Ok && w > 0 && h > 0 &&
                bmp.LockBits(&r, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &bd) == Gdiplus::Ok) {
                const int nw = w + kPlusW;
                std::vector<uint32_t> buf((size_t)nw * h, 0);
                for (int y = 0; y < h; ++y)
                    std::memcpy(buf.data() + (size_t)y * nw + kTextDx,
                                (const uint8_t*)bd.Scan0 + (size_t)y * bd.Stride, (size_t)w * 4);
                bmp.UnlockBits(&bd);
                drawPlus(buf.data(), nw, h);
                toTga(buf.data(), nw, h, g_pic);
                ok = true;
            }
        }
        s->Release();
    }
    Gdiplus::GdiplusShutdown(token);
    if (ok) serve("PICTURE", 193, g_pic.data(), g_pic.size());
    return ok;
}

HRSRC WINAPI h_find(HMODULE mod, LPCSTR name, LPCSTR type) {
    if (mod == g_fm8 && IS_INTRESOURCE(name) && !IS_INTRESOURCE(type)) {
        const int id = (int)(uintptr_t)name;
        if (g_wantPicture && id == 193 && _stricmp(type, "PICTURE") == 0) {
            g_wantPicture = false;      // one attempt; a failure just leaves FM8's own bitmap
            buildPicture();
        }
        for (int i = 0; i < g_count; ++i)
            if (g_entries[i].id == id && _stricmp(type, g_entries[i].type) == 0) return (HRSRC)&g_entries[i];
    }
    return o_find(mod, name, type);
}
DWORD   WINAPI h_size(HMODULE mod, HRSRC r) { Entry* e = ours(r); return e ? (DWORD)e->size : o_size(mod, r); }
HGLOBAL WINAPI h_load(HMODULE mod, HRSRC r) { return ours(r) ? (HGLOBAL)r : o_load(mod, r); }
LPVOID  WINAPI h_lock(HGLOBAL g)            { Entry* e = ours(g); return e ? (LPVOID)e->data : o_lock(g); }

} // namespace

void* patchImport(HMODULE mod, const char* dll, const char* func, void* replacement) {
    auto* base = (uint8_t*)mod;
    auto* dos = (IMAGE_DOS_HEADER*)base;
    auto* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return nullptr;
    for (auto* imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); imp->Name; ++imp) {
        if (_stricmp((const char*)(base + imp->Name), dll) != 0) continue;
        auto* names = (IMAGE_THUNK_DATA*)(base + imp->OriginalFirstThunk);
        auto* thunks = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++thunks) {
            if (names->u1.Ordinal & IMAGE_ORDINAL_FLAG) continue;
            auto* byName = (IMAGE_IMPORT_BY_NAME*)(base + names->u1.AddressOfData);
            if (std::strcmp(byName->Name, func) != 0) continue;
            DWORD old; void* orig = (void*)thunks->u1.Function;
            if (!VirtualProtect(&thunks->u1.Function, sizeof(void*), PAGE_READWRITE, &old)) return nullptr;
            thunks->u1.Function = (ULONGLONG)replacement;
            VirtualProtect(&thunks->u1.Function, sizeof(void*), old, &old);
            return orig;
        }
    }
    return nullptr;
}

bool install(HMODULE fm8) {
    if (g_fm8) return g_fm8 == fm8;
    const char* k32 = "KERNEL32.dll";
    o_find = (FindResourceA_t)patchImport(fm8, k32, "FindResourceA", (void*)&h_find);
    o_size = (SizeofResource_t)patchImport(fm8, k32, "SizeofResource", (void*)&h_size);
    o_load = (LoadResource_t)patchImport(fm8, k32, "LoadResource", (void*)&h_load);
    o_lock = (LockResource_t)patchImport(fm8, k32, "LockResource", (void*)&h_lock);
    if (!o_find || !o_size || !o_load || !o_lock) return false;   // ponytail: partial patch is left as is; FM8 imports all four
    g_fm8 = fm8;
    return true;
}

bool serveLogo() {
    if (!g_fm8) return false;
    // The bitmap is built on first use, which is past the point where we could withdraw the widened
    // form, so check up front that it is something GDI+ reads. FM8 ships 193 as a PNG; were it ever
    // anything else the GUI is left completely stock instead of gaining a 22px gap.
    static const uint8_t kPngSig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    size_t n = 0;
    auto* png = (const uint8_t*)stock("PICTURE", 193, n);
    if (!png || n < sizeof kPngSig || std::memcmp(png, kPngSig, sizeof kPngSig) != 0) return false;
    if (!widenForm(5, g_frm5) || !widenForm(15, g_frm15)) return false;
    serve("FRM", 5, g_frm5.data(), g_frm5.size());
    serve("FRM", 15, g_frm15.data(), g_frm15.size());
    g_wantPicture = true;
    return true;
}

} // namespace fm8plus::Rsrc
