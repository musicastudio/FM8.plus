# FM8 non-form GUI resources (PICTURE, PM, FNT, FM, TTFD)

Everything below was recovered from the Ghidra decompilation of `FM8.exe` (image base
0x140000000) and checked against the extracted resource bytes. Function addresses are given so
the claims can be re-verified with `python tools/q.py exe fn <addr>`.

All multi-byte integers in NI's own formats are little-endian (the stream is created by
`FUN_1408db5a0`, which calls `FUN_140901310(stream, 1)`; every reader such as `FUN_1408ff130`
byte-swaps only when that flag is not 1).

## 1. Resource inventory and the loading pipeline

PE resource types present: `PICTURE` (328), `PM` (5), `FNT` (46), `FM` (5), `FRM` (74),
`TTFD` (3: ids 1, 2, 5), plus RT_ICON/RT_GROUP_ICON/RT_VERSION/RT_MANIFEST.

Loading a resource always goes through the same path:

```
AppModule::GetResourceStream (FUN_1408db250 -> FUN_1408db5a0)(id, "TYPE", little_endian=1)
  FUN_1408f85f0: FindResourceA(hExe, MAKEINTRESOURCE(id & 0xffff), "TYPE") -> LoadResource -> LockResource
                 -> NI::GP::MemoryStorage over the locked bytes (FUN_1408fd780)
  fallback FUN_1408f8cf0: look (typeString, id) up in a compiled-in registry (FUN_140a04120);
                 nothing ever registers into it in FM8, so it throws "resource not found",
                 caught at Catch@140a4ecc0 -> cerr "cannot resolve resource: <type> <id>"
```

The Win32 resource id is the NI id, truncated to 16 bits. There is no per-module base and no
offset: the only offset globals (`DAT_141201b6c` for PM entry ids, `DAT_141201b68` for
ResourceRef ids) are read but never written, so they are 0.

The `"cannot resolve resource: resources_ENG" / "lib_ENG" / "application_ENG"` log lines come
from a different, harmless path: `FUN_1408fa200` builds `"<module>_<LANG>"` (`"_"` is
`DAT_140c18e40`, LANG is `DAT_141210160`, set by `FUN_1408f8780` from `GetUserDefaultLangID`
to ENG/DEU/SPA/FRA/ITA/JPN) and calls `FindResourceA(hExe, "<module>_ENG", "LOCA")` for the
localized string tables. FM8 ships no `LOCA` resources, the compiled-in fallback
(`FUN_1408f8d70`, handler `Catch@140a4ed30`) is empty, hence the message. It does not affect
pictures, fonts or forms.

Picture/font managers live in `NI::NGL::ResourceManager` instances (ctor `FUN_1407628f0`,
layout: +0x08 loaded flag, +0x10 PictureManager, +0x18 FontManager, +0x20 FRM override map,
+0x30 a second map). `ResourceManager::Init(pmId, fmId)` is `FUN_140766e40`: it creates a
`PictureManager` (`FUN_1407646d0`) and calls `PictureManager::Load(pmId)` (`FUN_140766880`,
which fetches resource `"PM"/pmId`), then creates a `FontManager` (`FUN_140764400`) and calls
`FontManager::Load(fmId)` (`FUN_140766d70`, resource `"FM"/fmId`). The pairs used by FM8 are
always equal and are the five PM/FM ids present in the PE:

| pm/fm id | where Init is called |
|---|---|
| 1 | main application (via the virtual id getter at vtable +0x88 in `FUN_140761ef0`) |
| 5000 | `FUN_1400d2ea0` (`Init(5000,5000)`) |
| 6000 | `FUN_1400c0a70` (`Init(6000,6000)`) |
| 8000 | `FUN_14012bac0` (`Init(8000,8000)`) |
| 20000 | `FUN_1400c0a70` (`Init(20000,20000)`) |

A manager's `Get(id)` (`ResourceManagerBase::Get`, `FUN_140763870`) looks the id up in its
`std::map<uint32, Entry*>`, creates a default entry if missing, and if the entry has no
resource yet calls the manager's `CreateResource` (vtable +0x48): `FUN_140764210` for
pictures (loads `"PICTURE"/id`), `FUN_140763fb0` for fonts (`"FNT"/id`). So any picture id
can be loaded through any manager; the PM/FM maps only pre-register ids with metadata.

Forms: `FRM/id` is fetched through `ResourceManager::GetStream` (`FUN_140766230`). For type
`"FRM"` only, it first consults the map at ResourceManager+0x20
(`map<uint32 id, map<string LANG, uint32 altId>>`) and, if the current language has an entry,
substitutes the alternate id. No code in FM8 inserts into that map, so form ids are used
as-is.

Named resources: the static table at 0x140ffcf08 is a NUL-terminated list of
`(const char* name, const char* type)` pairs: `THE_AB_PICTURE_MANAGER/"PM"`, 23 `IDP_*`
names with type `"PICTURE"` (IDP_CHECKBOX, IDP_SCROLLER, IDP_SCROLLER_BG, IDP_TEXTEDIT,
IDP_ARROW_DOUBLE, IDP_ABFONT_BLACK, IDP_FRAME_LIST, IDP_DROP_DOWNMENU, IDP_LONG_SLIDER256,
IDP_BTN_CANCEL, IDP_BTN_OK, IDP_HEADER_PATTERN, IDP_TABS_HEADER, IDP_NI_LOGO, IDP_STATUS_BG,
IDP_DOTTED_BAR, IDP_BG_OVERALL, IDP_SWITCH_BTN_LEFT_NEW, IDP_SWITCH_BTN_RIGHT,
IDP_FRAME_LIST_ROUTING, IDP_ASIO_NEW, IDP_ARROW_NEW2, IDP_ARROW_TRANSPARENT_NEW2),
`THE_AB_FONT_MANAGER/"FM"`, five `IDFNT_*` names with type `"FNT"` and three `IDF_AB_SETUP_*`
names with type `"FRM"`. Nothing in the code references the table or any of its strings (the
only xrefs are the table itself), and FM8 never calls `FindResourceA` with a string name for
these types. The IDP_ names are dead data from the shared "AB" setup-dialog library; they do
not map to ids in FM8.

## 2. PICTURE

`PictureManager::CreateResource` (`FUN_140764210`) builds a `NI::UIA::Picture` (0x80 bytes,
ctor `FUN_140791e70`) and calls `Picture::Load` (`FUN_140794cb0`), which tries three decoders
in order, rewinding the stream between attempts:

1. PNG via libpng 1.6.23 (`FUN_1407955a0`). Transforms: palette to RGB, 16 to 8 bit, gray to
   RGB, tRNS to alpha, `png_set_filler(0xff, AFTER)`, `png_set_bgr`.
2. JPEG via libjpeg v9 plus a Little-CMS transform to sRGB (`FUN_140795130`). No JPEG
   pictures exist in FM8.
3. TGA (`FUN_140795880`).

Of the 328 PICTUREs, 262 are PNG, 63 are TGA type 2 (uncompressed) and 3 (ids 6022, 6023,
6024) are TGA type 10 (RLE). The two "NI formats" are therefore plain Truevision TGA 2.0
files, including the optional 26-byte footer.

### 2.1 In-memory pixel format

`Picture` holds `uint32` pixels at +0x30, width at +0x38, height at +0x3c, rows top-down.
Each pixel is `0xAARRGGBB` as a little-endian u32, i.e. bytes `B G R A` in memory (the PNG
path uses `png_set_bgr`, the TGA path copies the file's BGRA bytes verbatim).

### 2.2 TGA grammar as read by FUN_140795880

```
offset size  field                 accepted values
0      u8    id_length             any (FM8 files: 0)
1      u8    colour_map_type       must be 0 (no palette support)
2      u8    image_type            2 = uncompressed true colour, 10 = RLE true colour
3      u16   cmap_first            ignored
5      u16   cmap_length           ignored
7      u8    cmap_entry_bits       ignored
8      u16   x_origin              ignored
10     u16   y_origin              ignored
12     u16   width
14     u16   height
16     u8    pixel_depth           16, 24 or 32
17     u8    descriptor            bit 5 (0x20) = rows stored top-down; FM8 files: 0x08 (32-bit)
                                   or 0x00 (24-bit), i.e. bottom-up
18     u8[id_length] image id      skipped
then   pixel data, width*height pixels, in file row order
end    26-byte TGA 2.0 footer (ext offset u32, dev offset u32, "TRUEVISION-XFILE.\0"),
       present in every FM8 file, not read by the loader
```

Pixel data:

* depth 32: 4 bytes per pixel `B G R A`. Type 2: raw. Type 10: packets, header byte `h`;
  if `h & 0x80` the next 4 bytes repeat `(h & 0x7f) + 1` times, else the next
  `(h & 0x7f) + 1` pixels (4 bytes each) are literal. Packets never cross the end of the
  image but may cross row boundaries. RLE is only implemented for 32-bit.
* depth 24: `B G R`, expanded to `0xFF RR GG BB`.
* depth 16: little-endian `A RRRRR GGGGG BBBBB`; `0x0000` becomes fully transparent black
  (`0x00000000`), anything else becomes `0xFF (R<<3) (G<<3) (B<<3)` (the low 3 bits are not
  replicated).
* After decoding, if descriptor bit 5 is clear the rows are flipped in place
  (`FUN_1407948c0`), so the result is top-down.

Every FM8 TGA is `w*h*bpp + 18 + 26` bytes (type 2) or smaller (type 10); e.g. id 5008 is
1448x12x4 = 69504 + 44 = 69548 bytes.

### 2.3 Alpha detection

`Load` receives a "detect alpha" flag (true only when the picture has no PM entry, see below).
When set, the PNG decoder marks the picture as having alpha if the colour type is 3, 4 or 6
and any pixel has alpha != 0xff; the TGA decoder does the same for 32-bit, or for 16-bit if
any pixel is 0. 24-bit TGA is opaque.

## 3. PM ("picture map"): PictureManager manifest

Resource `PM/<moduleId>`, deserialized by `ResourceManagerBase::Deserialize`
(`FUN_140767080`) with per-entry `PictureEntry::Deserialize` (`FUN_140767230` ->
`PictureProperties::Deserialize` `FUN_140794c20`; the matching serializer is `FUN_140795f80`).

```
u32 version        must be >= 3, else the map is ignored (file value: 3)
u32 count
count x record (37 bytes each):
  u32 picture_id   (+ DAT_141201b6c, which is 0)
  u32 props_version  ignored on read, written as 1
  u8  has_alpha    PictureProperties+0x08
  u32 orientation  +0x0c: 1 = frames stacked vertically, 2 = frames side by side (default 1)
  u32 frames       +0x10: number of animation/state frames in the strip (0 or 1 = single)
  u32 stretch      +0x14: bit0 (1) = stretch vertically, bit1 (2) = stretch horizontally
  i32 top          +0x18: fixed top band height    (used only when stretch bit0 set)
  i32 bottom       +0x1c: fixed bottom band height (bit0)
  i32 left         +0x20: fixed left band width    (bit1)
  i32 right        +0x24: fixed right band width   (bit1)
```

File sizes check out: 8 + 37*count for all five (PM/1 213 records, PM/5000 23, PM/6000 69,
PM/8000 18, PM/20000 5; 328 records, exactly one per PICTURE id, no orphans either way).

Semantics, from `PictureEntry::SetResource` (`FUN_140767900`, which pushes the entry
properties into the loaded `Picture` through vtable slots +0x08..+0x50) and the stretch-blit
`FUN_140792c90(gfx, destRect, picture, frameIndex, srcRect)`:

* `has_alpha` overrides whatever the decoder found. If an entry came from a PM the loader is
  called with detect=false and the PM flag wins (entry+0x40 is set by the entry factory and
  cleared by `PictureEntry::Deserialize`). A picture with `has_alpha = 0` is composited as
  opaque even if the PNG carries alpha.
* Frame size: `orientation == 1` gives frames of `w x (h / frames)` at
  `y = (h / frames) * index`; otherwise `(w / frames) x h` at `x = (w / frames) * index`.
  `index` is clamped to `frames - 1`; `frames <= 1` means the whole picture. Examples:
  id 7 is 29x3712 with 128 vertical frames (29x29 knob), id 2 is 1218x219 with 203
  horizontal frames of 6x219, id 6000 is 24x168 with 7 frames of 24x24.
* Stretching is a 9-slice: when the destination rect is not the frame size, corners are
  blitted 1:1 (`FUN_140792540`), the four edges and the centre are scaled/tiled
  (`FUN_140792760`) to fill `dest - fixed bands`. Only the bands of an enabled axis are used;
  with a bit clear that axis's bands are 0 and the whole axis scales. If the destination
  equals the frame size the fast path `FUN_140792b50` just copies the frame. Example: id 6002
  (97x45) has stretch 3 with top 17, bottom 4, left 69, right 4.

## 4. FNT: NI::NGL::Font

`FontManager::CreateResource` (`FUN_140763fb0`, `font_ngl.cpp` line 0x1db, "Invalid Font
resource version!") builds an `NI::NGL::Font` (0xe8 bytes, ctor `FUN_140762740`) and reads:

```
u32 version              must be <= 3 (FM8: 2 or 3)
u32 type                 Font+0x08: 0 = picture font, 2 = TrueType font (1 = neither, no glyph source)
ResourceRef<Picture>     Font+0x20: { u32 ref_version (1), u32 picture_id }  -> +0x28  (glyph strip)
ResourceRef<Picture>     Font+0x38: { u32, u32 picture_id }                  -> +0x40  (second glyph strip, alternate style)
ResourceRef<Picture>     Font+0x80: { u32, u32 picture_id }                  -> +0x88  (extra picture, always 0 in FM8)
u32 colour0              Font+0x74, 0xAARRGGBB, default 0xffffffff
u32 colour1              Font+0x78, 0xAARRGGBB, default 0xff000000
if version >= 2 and type == 2:
  u32 face_id            Font+0x54: TTFD resource id (default 1)
  u32 pixel_size         Font+0x5c: FreeType pixel size (default 10)
  u32 colour2            Font+0x6c, 0xAARRGGBB, default 0xff000000
  u32 extra              Font+0x70, default 0 (FM8: 0 or 0xffffffff, purpose not traced)
if version >= 3:
  u32 metric_mode        Font+0x64, default 3, copied to the glyph renderer's +0x14
  u32 render_mode        Font+0x68, default 3, copied to the glyph renderer's +0x18
```

Sizes: version 3 picture font = 48 bytes (44 of 46 files), version 2 picture font = 40 bytes
(FNT/5002), version 2 TrueType = 56 bytes (FNT/5000, 5001, 5003, 5004). `ResourceRef`
deserialization is `FUN_140745fc0` (reads and discards a version u32, then the id, plus the
always-zero `DAT_141201b68`).

Realization (`FUN_140766b60`):

* type 0: `PictureManager::Get(picture_id)` for +0x28 and, if non-zero, +0x40; each becomes a
  `DETAIL::PictureFont` (0x828 bytes, ctor `FUN_140762890`, init `FUN_140763bb0`).
* type 2: `SizedFontDataManager::Get((pixel_size << 24) | face_id)` ->
  `FUN_140764300`: `FontDataManager::Get(face_id)` -> `FUN_140763e70` loads resource
  `"TTFd"/face_id` (the PE type `TTFD`, FindResourceA is case-insensitive) as a raw blob,
  then `FT_New_Memory_Face`, `FT_Select_Charmap('unic')`, `FT_Set_Pixel_Sizes(size, 0)`.
  Result wrapped in `DETAIL::TrueTypeFont` (`FUN_140764760`, ascender scaled by
  `DAT_140ab48a8`). `metric_mode` 3 uses FreeType 26.6 advances directly, 0/1/2 use
  rounded scaled advances (`FUN_140765e10`); `render_mode` is passed to the glyph loader
  (`FUN_140765810`).
* Fallback: if the FNT resource is missing and FontManager+0x28 is set (it is 0 in FM8), the
  id is used directly as a PICTURE id for a picture font.

### 4.1 Picture font layout (FUN_140763bb0)

The glyph strip covers character codes 0..255 left to right. The loader counts non-zero
pixels in row 0:

* exactly 256 non-zero pixels: proportional font. Row 0 is a marker row; each non-zero pixel
  marks the start column of the next glyph, and a glyph extends until the next marker. Glyph
  height = picture height - 1, glyph rows are 1..h-1. (All FM8 picture fonts are like this;
  e.g. id 5008 is 1448x12: 256 glyphs of height 11.)
* otherwise: fixed-width font, cell width = width / 256, glyph height = height; the picture is
  re-tagged as 256 horizontal frames.

Glyph blit is `FUN_140764d90` -> `FUN_140793230(dest, x, y, glyphW, glyphH, picture, srcX,
fixed)`; characters >= 256 draw the glyph for `?` (0x3f). Colour0/colour1 are not consumed by
the blit itself; they are the font's two colours available to the control drawing code (in
FM8 the pairs are things like white / white at alpha 0x50, or grey 0x585a5a opaque and at
alpha 0x50).

### 4.2 FM8 font inventory

Module 1 (ids 2..19, 39): all picture fonts, glyph strips PICTURE 88, 164, 157..162, 62, 67,
77, 78, 82, 86, 39 (FNT/2 has no strip at all). Module 5000: FNT/5000, 5001, 5003, 5004 are
TrueType face TTFD/2 at 13, 12, 12, 13 px; FNT/5002 is a picture font on PICTURE 5008.
Module 6000: picture fonts on 6026 (most), 6027, 6046, 6051, 6052, 6054 (+6052 as second
strip), 6055, 6057, 6062, 6065. Module 8000: picture fonts on 8000, 8001, 8003, 8005, 8007,
8006; FNT/10000 on PICTURE 10000. TTFD/1 and TTFD/5 are not referenced by any FNT.

## 5. FM ("font map"): FontManager manifest

Same container as PM (`FUN_140767080`), per-entry body from
`ResourceEntry<Font>::Deserialize` (`FUN_140767040`), which reads one u32 and ignores it
(the serializer `FUN_140767a40` writes 0):

```
u32 version   >= 3 (file value 3)
u32 count
count x { u32 font_id; u32 zero }
```

Sizes: 8 + 8*count (FM/1 18 entries: 2..10, 12..19, 39; FM/5000: 5000..5004; FM/6000: 16
entries; FM/8000: 8000..8005 and 10000; FM/20000: 0 entries). An FM entry only pre-creates
an empty `ResourceEntry<Font>`; the FNT is loaded on first `Get`.

## 6. TTFD

Raw TrueType files (sfnt version `00 01 00 00`), loaded verbatim by `FUN_140763e70`. Ids 1
(2,677,208 bytes), 2 (4,000,340 bytes) and 5 (195,796 bytes). Not NI-specific.

## 7. Python: decoding and parsing

```python
import struct, zlib

def decode_tga(data):
    """FM8 PICTURE stored as TGA (types 2/10, 16/24/32 bpp). Returns (w, h, rgba bytes, top-down)."""
    idlen, cmtype, imtype, _, _, _, _, _, w, h, depth, desc = struct.unpack_from("<BBBHHBHHHHBB", data, 0)
    assert imtype in (2, 10) and cmtype == 0 and depth in (16, 24, 32)
    p, bpp, n = 18 + idlen, depth // 8, w * h
    px = bytearray()
    if imtype == 2:
        px += data[p:p + n * bpp]
    else:                                   # RLE packets (32-bit only in FM8)
        while len(px) < n * bpp:
            hdr = data[p]; p += 1; cnt = (hdr & 0x7f) + 1
            if hdr & 0x80:
                px += data[p:p + bpp] * cnt; p += bpp
            else:
                px += data[p:p + bpp * cnt]; p += bpp * cnt
    out = bytearray(n * 4)
    for i in range(n):
        s = i * bpp
        if bpp == 4:
            b, g, r, a = px[s], px[s + 1], px[s + 2], px[s + 3]
        elif bpp == 3:
            b, g, r, a = px[s], px[s + 1], px[s + 2], 255
        else:
            v = px[s] | (px[s + 1] << 8)
            if v == 0:
                r = g = b = a = 0
            else:
                r, g, b, a = ((v >> 10) & 31) << 3, ((v >> 5) & 31) << 3, (v & 31) << 3, 255
        out[i * 4:i * 4 + 4] = bytes((r, g, b, a))
    if not (desc & 0x20):                   # bottom-up file: flip
        out = b"".join(out[y * w * 4:(y + 1) * w * 4] for y in range(h - 1, -1, -1))
    return w, h, bytes(out)

def decode_picture(data):
    """PNG -> hand to any PNG decoder; otherwise TGA."""
    if data[:8] == b"\x89PNG\r\n\x1a\n":
        return ("png", data)
    return ("rgba",) + decode_tga(data)

def parse_pm(data):
    ver, n = struct.unpack_from("<II", data, 0); assert ver >= 3
    recs, o = {}, 8
    for _ in range(n):
        pid, _pver, has_alpha, orient, frames, stretch = struct.unpack_from("<IIBIII", data, o); o += 21
        top, bottom, left, right = struct.unpack_from("<iiii", data, o); o += 16
        recs[pid] = dict(has_alpha=has_alpha, orientation=orient, frames=frames,
                         stretch_v=bool(stretch & 1), stretch_h=bool(stretch & 2),
                         top=top, bottom=bottom, left=left, right=right)
    return recs

def parse_fm(data):
    ver, n = struct.unpack_from("<II", data, 0); assert ver >= 3
    return [struct.unpack_from("<II", data, 8 + 8 * i)[0] for i in range(n)]

def parse_fnt(data):
    ver, typ = struct.unpack_from("<II", data, 0); assert ver <= 3
    o = 8
    refs = []
    for _ in range(3):
        _rv, rid = struct.unpack_from("<II", data, o); o += 8; refs.append(rid)
    f = dict(version=ver, type=typ, glyph_picture=refs[0], alt_picture=refs[1], extra_picture=refs[2])
    f["colour0"], f["colour1"] = struct.unpack_from("<II", data, o); o += 8
    if ver >= 2 and typ == 2:
        f["ttfd_id"], f["pixel_size"], f["colour2"], f["extra"] = struct.unpack_from("<IIII", data, o); o += 16
    if ver >= 3:
        f["metric_mode"], f["render_mode"] = struct.unpack_from("<II", data, o); o += 8
    return f

def frame_rect(w, h, rec, index):
    """Sub-rectangle (x, y, fw, fh) of frame `index` given a parse_pm record."""
    n = max(rec["frames"], 1); index = min(index, n - 1)
    if rec["orientation"] == 1:
        fh = h // n; return 0, fh * index, w, fh
    fw = w // n; return fw * index, 0, fw, h

def write_png(path, w, h, rgba):
    raw = b"".join(b"\x00" + rgba[y * w * 4:(y + 1) * w * 4] for y in range(h))
    def chunk(t, d): return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
    with open(path, "wb") as fp:
        fp.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
                 + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))
```

Proof: running `decode_tga` on the extracted bytes produced `pic5008.png` (1448x12, alpha
values {0, 255}) and `pic6022.png` (161x52 from the 1120-byte RLE file, alpha values
{64, 255}) in the scratch folder `agentB`. Viewed, 5008 is the picture-font glyph strip
(Latin-1 order, `!"#$%&'()*+,-./0123456789:;<=>?@ABC...` with a marker row on top, matching
FNT/5002) and 6022 is a grey panel with two orange horizontal bars (the RLE compresses the
flat colour well). 24-bit (6003) and 32-bit-with-alpha (6000) files also decoded to the
expected sizes.
