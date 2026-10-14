// "Sharp bilinear" (area coverage) scaling of FM8's 32-bit surface for the fractional GUI Scale
// steps, shared by Windows (core/fm8plus.cpp) and the Mac (mac/core_mac.cpp).
//
// The usual answer for pixel art at a non-integer scale: every logical pixel covers exactly k
// physical ones, and only a physical pixel that a logical pixel boundary passes through mixes the
// two it straddles, by how much of each it covers. Text strokes keep one even width and flat areas
// stay crisp, where a general filter (GDI's HALFTONE, CoreGraphics' interpolation) smears
// everything and nearest neighbour alternates 1- and 2-pixel columns. The user picked it from a
// side-by-side (2026-10-13) over HALFTONE, bilinear, Lanczos, linear-light and Scale3x variants.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace fm8plus {

// (a*p + (256-a)*q) / 256 for each byte of two pixels.
inline uint32_t mixPixel(uint32_t p, uint32_t q, uint32_t a) {
    if (a >= 256) return p;
    const uint32_t b = 256 - a;
    const uint32_t rb = ((((p & 0x00ff00ff) * a + (q & 0x00ff00ff) * b) + 0x00800080) >> 8) & 0x00ff00ff;
    const uint32_t ag = ((((p >> 8) & 0x00ff00ff) * a + ((q >> 8) & 0x00ff00ff) * b) + 0x00800080) & 0xff00ff00;
    return rb | ag;
}

// Fill `out` (w x h pixels, top row first) with physical pixels x0..x0+w, y0..y0+h of the surface
// `src` (W x H, top row at `src`, rows `stride` bytes apart, so negative for a bottom-up bitmap)
// scaled by k, where the surface's top-left sits `ox,oy` logical pixels from the physical origin.
// Each physical pixel depends only on its own position, so a repaint of part of the GUI lands on
// exactly the pixels a repaint of the whole of it would.
inline void scaleSharp(const uint8_t* src, int W, int H, ptrdiff_t stride, float k, int ox, int oy,
                       int x0, int y0, int w, int h, uint32_t* out) {
    if (w <= 0 || h <= 0 || W <= 0 || H <= 0) return;
    // Per physical column (and row): the first source pixel it covers, and that pixel's share /256.
    struct Tap { int j; uint32_t a; };
    auto taps = [k](int from, int n, int o, int limit, std::vector<Tap>& t) {
        t.resize(n);
        for (int i = 0; i < n; ++i) {
            const float a = (from + i) / k - o, b = (from + i + 1) / k - o;   // its span in source pixels
            const int j = (int)std::floor(a);
            const float share = (std::min((float)j + 1, b) - a) * k;
            t[i] = j < 0 ? Tap{0, 256} : j >= limit - 1 ? Tap{limit - 1, 256} : Tap{j, (uint32_t)std::lround(share * 256)};
        }
    };
    static thread_local std::vector<Tap> tx, ty;
    static thread_local std::vector<uint32_t> rowA, rowB;
    taps(x0, w, ox, W, tx);
    taps(y0, h, oy, H, ty);
    // Each source row is scaled across once and kept while the physical rows that use it pass; only
    // rows on a pixel boundary then mix two of them.
    rowA.resize(w); rowB.resize(w);
    int idxA = -1, idxB = -1;
    auto across = [&](int sy) -> const uint32_t* {
        if (sy == idxA) return rowA.data();
        if (sy == idxB) return rowB.data();
        const bool useA = idxA < idxB;   // rows only move down: replace the older one
        std::vector<uint32_t>& dst = useA ? rowA : rowB;
        (useA ? idxA : idxB) = sy;
        const auto* s = (const uint32_t*)(src + sy * stride);
        for (int x = 0; x < w; ++x) dst[x] = mixPixel(s[tx[x].j], s[std::min(tx[x].j + 1, W - 1)], tx[x].a);
        return dst.data();
    };
    for (int y = 0; y < h; ++y) {
        uint32_t* o = out + (size_t)y * w;
        const uint32_t* r0 = across(ty[y].j);
        if (ty[y].a >= 256) { std::copy(r0, r0 + w, o); continue; }
        const uint32_t* r1 = across(std::min(ty[y].j + 1, H - 1));
        for (int x = 0; x < w; ++x) o[x] = mixPixel(r0[x], r1[x], ty[y].a);
    }
}

} // namespace fm8plus
