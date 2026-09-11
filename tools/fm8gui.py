#!/usr/bin/env python3
"""FM8 GUI layout tooling: extract, decompile (FRM -> XML), recompile (XML -> FRM), patch, preview.

FM8 stores every window layout as an "FRM" resource in the PE .rsrc (74 forms), the artwork as
"PICTURE" resources (PNG or TGA), fonts as "FNT" descriptors (picture-font glyph strips or TrueType
"TTFD" blobs). A form is a header plus a list of typed control records (docs/gui-frm-grammar.md);
SubForm controls embed other forms by id, so the 948x562 editor (FRM 3) is a tree.

    python tools/fm8gui.py extract [pe] [outdir]       dump FRM/PICTURE/FNT/FM/PM/TTFD resources to files
    python tools/fm8gui.py dump    [rsrcdir] [xmldir]  decompile every FRM to <xmldir>/<id>.xml
    python tools/fm8gui.py build   <xml> <out.bin>     recompile one XML form to an FRM blob
    python tools/fm8gui.py check   [rsrcdir]           parse + rebuild every FRM and byte-compare
    python tools/fm8gui.py preview [rsrcdir] [outdir]  render every form (nested, real artwork) to PNG + index.html
    python tools/fm8gui.py pictures [rsrcdir] [outdir] contact sheet of every PICTURE with its PM strip metadata
    python tools/fm8gui.py patch   <pe-in> <pe-out> <id> <frm.bin>   write a FRM into a COPY of a PE
    python tools/fm8gui.py embed   <xml|bin> <out.h> [name]        C header for Rsrc::overrideForm (runtime serving)

Options: --xml DIR renders from XML files instead of the extracted binaries (preview your edits);
--wire adds class-coloured outlines. Defaults: pe = the FM8_DISASM FM8.exe; everything else under build/gui/.
"""
from __future__ import annotations

import base64
import glob
import io
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import frm_grammar as G  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
DEFAULT_PE = os.path.join(os.path.dirname(ROOT), "FM8_DISASM", "FM8_EXE", "FM8.exe")
GUI_DIR = os.path.join(ROOT, "build", "gui")
TYPES = ("FRM", "PICTURE", "FNT", "FM", "PM", "TTFD")

# --- resources ---------------------------------------------------------------------------------

def extract(pe_path=DEFAULT_PE, out=None):
    import pefile
    out = out or os.path.join(GUI_DIR, "rsrc")
    pe = pefile.PE(pe_path, fast_load=True)
    pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_RESOURCE"]])
    n = 0
    for t in pe.DIRECTORY_ENTRY_RESOURCE.entries:
        tname = t.name.string.decode() if t.name else pefile.RESOURCE_TYPE.get(t.id, str(t.id))
        if tname not in TYPES:
            continue
        os.makedirs(os.path.join(out, tname), exist_ok=True)
        for e in t.directory.entries:
            nm = e.name.string.decode() if e.name else str(e.id)
            for lang in e.directory.entries:
                data = pe.get_data(lang.data.struct.OffsetToData, lang.data.struct.Size)
                ext = ".png" if data.startswith(b"\x89PNG") else ".bin"
                with open(os.path.join(out, tname, nm + ext), "wb") as f:
                    f.write(data)
                n += 1
    print(f"extracted {n} resources to {out}")
    return out


def load_frm(rsrc, fid):
    p = os.path.join(rsrc, "FRM", f"{fid}.bin")
    return open(p, "rb").read() if os.path.exists(p) else None


def form_ids(rsrc):
    return sorted(int(os.path.basename(p)[:-4]) for p in glob.glob(os.path.join(rsrc, "FRM", "*.bin")))


parse, build, to_xml, from_xml = G.parse, G.serialize, G.to_xml, G.from_xml


class Forms:
    """Form source: extracted binaries, optionally overridden by XML files (for previewing edits)."""
    def __init__(self, rsrc, xmldir=None):
        self.rsrc, self.xmldir, self.cache = rsrc, xmldir, {}

    def ids(self):
        ids = set(form_ids(self.rsrc))
        if self.xmldir:
            ids |= {int(os.path.basename(p)[:-4]) for p in glob.glob(os.path.join(self.xmldir, "*.xml"))}
        return sorted(ids)

    def get(self, fid):
        if fid not in self.cache:
            form = None
            xp = os.path.join(self.xmldir, f"{fid}.xml") if self.xmldir else None
            if xp and os.path.exists(xp):
                form = from_xml(open(xp, encoding="utf-8").read())
                for c in form["controls"]:
                    c["_class"] = G.grammar_class(c)
            elif load_frm(self.rsrc, fid):
                form = parse(load_frm(self.rsrc, fid))
            self.cache[fid] = form
        return self.cache[fid]


def dump(rsrc=None, xmldir=None):
    rsrc = rsrc or os.path.join(GUI_DIR, "rsrc")
    xmldir = xmldir or os.path.join(GUI_DIR, "xml")
    os.makedirs(xmldir, exist_ok=True)
    for fid in form_ids(rsrc):
        with open(os.path.join(xmldir, f"{fid}.xml"), "w", encoding="utf-8") as f:
            f.write(to_xml(fid, parse(load_frm(rsrc, fid))))
    print(f"wrote {len(form_ids(rsrc))} forms to {xmldir}")


def check(rsrc=None):
    rsrc = rsrc or os.path.join(GUI_DIR, "rsrc")
    bad = 0
    for fid in form_ids(rsrc):
        b = load_frm(rsrc, fid)
        try:
            again = build(from_xml(to_xml(fid, parse(b))))
            if again != b:
                raise ValueError("bytes differ after XML round trip")
        except Exception as e:  # noqa: BLE001
            bad += 1
            print(f"FRM {fid}: {e}")
    print(f"{len(form_ids(rsrc)) - bad}/{len(form_ids(rsrc))} forms round-trip byte-exact through XML")
    return bad == 0


# --- pictures and fonts (formats in docs/gui-resources.md) --------------------------------------
PM_REC = struct.Struct("<IIBIIIiiii")   # 37 bytes


def parse_pm(data):
    ver, n = struct.unpack_from("<II", data, 0)
    recs = {}
    for i in range(n):
        pid, _pver, alpha, orient, frames, stretch, top, bottom, left, right = PM_REC.unpack_from(data, 8 + 37 * i)
        recs[pid] = {"alpha": alpha, "orientation": orient, "frames": frames, "stretch": stretch,
                     "bands": (top, bottom, left, right)}
    return recs


def parse_fnt(data):
    ver, typ = struct.unpack_from("<II", data, 0)
    refs = [struct.unpack_from("<II", data, 8 + 8 * i)[1] for i in range(3)]
    o = 32
    f = {"version": ver, "type": typ, "glyph_picture": refs[0], "alt_picture": refs[1]}
    f["colour0"], f["colour1"] = struct.unpack_from("<II", data, o); o += 8
    if ver >= 2 and typ == 2:
        f["ttfd"], f["px"], f["colour2"], f["extra"] = struct.unpack_from("<IIII", data, o); o += 16
    if ver >= 3:
        f["metric_mode"], f["render_mode"] = struct.unpack_from("<II", data, o)
    return f


class Assets:
    """Pictures (with PM strip metadata) and fonts of one extracted resource tree, cached."""
    def __init__(self, rsrc):
        self.rsrc = rsrc
        self.pm = {}
        for p in glob.glob(os.path.join(rsrc, "PM", "*.bin")):
            self.pm.update(parse_pm(open(p, "rb").read()))
        self.fonts = {int(os.path.basename(p)[:-4]): parse_fnt(open(p, "rb").read())
                      for p in glob.glob(os.path.join(rsrc, "FNT", "*.bin"))}
        self._pics, self._glyphs, self._ttf = {}, {}, {}

    def picture(self, pid):
        from PIL import Image
        if pid not in self._pics:
            img = None
            for ext, fmts in ((".png", None), (".bin", ["TGA"])):
                p = os.path.join(self.rsrc, "PICTURE", f"{pid}{ext}")
                if os.path.exists(p):
                    img = Image.open(p, formats=fmts).convert("RGBA")
                    break
            self._pics[pid] = img
        return self._pics[pid]

    def frame(self, pid, index=0):
        """Frame `index` of picture strip `pid` per its PM record (orientation 1 = vertical stack)."""
        img = self.picture(pid)
        if img is None:
            return None
        rec = self.pm.get(pid, {})
        n = max(rec.get("frames", 1), 1)
        index = max(0, min(index, n - 1))
        w, h = img.size
        if rec.get("orientation") == 2:
            fw = w // n
            return img.crop((fw * index, 0, fw * index + fw, h))
        fh = h // n
        return img.crop((0, fh * index, w, fh * index + fh))

    def glyphs(self, pid):
        """Picture font: glyph strip -> {char code: image}. Row 0 with exactly 256 set pixels marks
        glyph starts (proportional); otherwise fixed cells of width/256."""
        if pid not in self._glyphs:
            img = self.picture(pid)
            if img is None:
                self._glyphs[pid] = None
            else:
                w, h = img.size
                px = img.load()
                starts = [x for x in range(w) if px[x, 0] != (0, 0, 0, 0)]
                if len(starts) == 256:
                    ends = starts[1:] + [w]
                    self._glyphs[pid] = {i: img.crop((starts[i], 1, ends[i], h)) for i in range(256)}
                else:
                    cw = max(w // 256, 1)
                    self._glyphs[pid] = {i: img.crop((cw * i, 0, cw * i + cw, h)) for i in range(256)}
        return self._glyphs[pid]

    def truetype(self, ttfd_id, px):
        from PIL import ImageFont
        key = (ttfd_id, px)
        if key not in self._ttf:
            p = os.path.join(self.rsrc, "TTFD", f"{ttfd_id}.bin")
            self._ttf[key] = ImageFont.truetype(io.BytesIO(open(p, "rb").read()), px) if os.path.exists(p) else None
        return self._ttf[key]


def gallery(rsrc=None, out=None):
    """Contact sheet of every picture with its PM metadata and the fonts that use it."""
    rsrc = rsrc or os.path.join(GUI_DIR, "rsrc")
    out = out or os.path.join(GUI_DIR, "preview")
    os.makedirs(out, exist_ok=True)
    a = Assets(rsrc)
    used_by = {}
    for fid, f in a.fonts.items():
        for k in ("glyph_picture", "alt_picture"):
            if f[k]:
                used_by.setdefault(f[k], []).append(f"FNT {fid}")
    ids = sorted({int(os.path.basename(p).split(".")[0]) for p in glob.glob(os.path.join(rsrc, "PICTURE", "*"))})
    rows = []
    for pid in ids:
        img, rec = a.picture(pid), a.pm.get(pid, {})
        buf = io.BytesIO(); a.frame(pid).save(buf, "PNG")
        b64 = base64.b64encode(buf.getvalue()).decode()
        rows.append(f"<tr><td>{pid}</td><td>{img.width}x{img.height}</td><td>{rec.get('frames', '')}</td>"
                    f"<td>{'h' if rec.get('orientation') == 2 else 'v'}</td><td>{rec.get('stretch', '')} {rec.get('bands', '')}</td>"
                    f"<td>{', '.join(used_by.get(pid, []))}</td><td><img src=\"data:image/png;base64,{b64}\" title=\"frame 0\"></td></tr>")
    html = ("<!doctype html><meta charset=utf-8><title>FM8 pictures</title><style>body{font:13px system-ui;margin:16px}"
            "td{border-bottom:1px solid #ddd;padding:3px 8px;vertical-align:middle}img{background:#c9c;max-width:600px}</style>"
            "<h1>FM8 pictures (frame 0 shown; see PM columns for strips)</h1><table><tr><th>id<th>size<th>frames<th>dir<th>stretch/bands<th>font use<th>image</tr>"
            + "".join(rows) + "</table>")
    with open(os.path.join(out, "pictures.html"), "w", encoding="utf-8") as f:
        f.write(html)
    print(f"wrote {len(ids)} pictures to {os.path.join(out, 'pictures.html')}")


# --- renderer -----------------------------------------------------------------------------------
CLASS_COLOURS = {"Label": (60, 120, 220), "SubForm": (200, 60, 60), "Switch": (40, 160, 90), "ValueEdit": (220, 140, 30),
                 "Selector": (150, 60, 200), "TextEdit": (0, 160, 180), "ButtonMenu": (180, 40, 120),
                 "Scope": (90, 90, 90), "LevelMonitor": (90, 90, 90), "List2": (120, 120, 40), "Generic": (0, 0, 0),
                 "ShadeArea": (120, 120, 120), "Tree": (120, 120, 40)}


def rgba(argb):
    return ((argb >> 16) & 255, (argb >> 8) & 255, argb & 255, (argb >> 24) & 255)


def blit(img, src, x, y):
    """Alpha-composite `src` onto `img` at (x, y), clipped to the canvas."""
    if src is None:
        return
    w, h = img.size
    sx, sy = max(0, -x), max(0, -y)
    ex, ey = min(src.width, w - x), min(src.height, h - y)
    if ex <= sx or ey <= sy:
        return
    img.alpha_composite(src.crop((sx, sy, ex, ey)), (x + sx, y + sy))


def stretch9(src, size, bands, stretch):
    """Scale a picture frame to `size` the way NGL does: fixed corner/edge bands on each axis whose
    stretch bit is set (bit0 vertical: top/bottom; bit1 horizontal: left/right), the rest resized."""
    from PIL import Image
    W, H = size
    top, bottom, left, right = bands
    if not stretch & 1:
        top = bottom = 0
    if not stretch & 2:
        left = right = 0
    xs = [0, left, src.width - right, src.width]
    ys = [0, top, src.height - bottom, src.height]
    dx = [0, left, W - right, W]
    dy = [0, top, H - bottom, H]
    out = Image.new("RGBA", (max(W, 1), max(H, 1)), (0, 0, 0, 0))
    for j in range(3):
        for i in range(3):
            sw, sh = xs[i + 1] - xs[i], ys[j + 1] - ys[j]
            tw, th = dx[i + 1] - dx[i], dy[j + 1] - dy[j]
            if sw <= 0 or sh <= 0 or tw <= 0 or th <= 0:
                continue
            part = src.crop((xs[i], ys[j], xs[i + 1], ys[j + 1]))
            if (sw, sh) != (tw, th):
                part = part.resize((tw, th), Image.NEAREST)
            out.alpha_composite(part, (dx[i], dy[j]))
    return out


class Renderer:
    def __init__(self, forms, assets, wire=False):
        self.forms, self.a, self.wire = forms, assets, wire

    def panel(self, img, rect, panel, frame=0):
        from PIL import ImageDraw
        x1, y1, x2, y2 = rect
        mode = panel["mode"]
        if mode == 1:
            fill = rgba(panel["colour"])
            if fill[3]:
                layer = img.crop((0, 0, 0, 0))  # placeholder to keep PIL happy on empty rects
                ImageDraw.Draw(img, "RGBA").rectangle((x1, y1, x2 - 1, y2 - 1), fill=fill)
        elif mode in (2, 3):
            pid = panel["picture"]["id"]
            fr = self.a.frame(pid, frame)
            if fr is None:
                return
            rec = self.a.pm.get(pid, {})
            size = (x2 - x1, y2 - y1)
            if fr.size != size and rec.get("stretch"):
                fr = stretch9(fr, size, rec["bands"], rec["stretch"])
            blit(img, fr, x1, y1)

    def text(self, img, rect, ti, margins=(0, 0, 0, 0)):
        s = ti["text"].replace("\\n", "\n")
        if not s:
            return
        x1, y1, x2, y2 = rect[0] + margins[0], rect[1] + margins[2], rect[2] - margins[1], rect[3] - margins[3]
        fnt = self.a.fonts.get(ti["font"]["id"])
        lines = s.split("\n")
        rendered = []
        for line in lines:
            im = self.line(line, fnt)
            if im is not None:
                rendered.append(im)
        if not rendered:
            return
        total_h = sum(im.height for im in rendered)
        y = y1 if ti["vAlign"] == 0 else (y1 + y2 - total_h) // 2 if ti["vAlign"] == 1 else y2 - total_h
        for im in rendered:
            x = x1 if ti["hAlign"] == 0 else (x1 + x2 - im.width) // 2 if ti["hAlign"] == 1 else x2 - im.width
            blit(img, im, x, y)
            y += im.height

    def line(self, s, fnt):
        from PIL import Image, ImageDraw
        if fnt and fnt["type"] == 2 and fnt.get("ttfd"):
            ttf = self.a.truetype(fnt["ttfd"], fnt["px"])
            if ttf:
                box = ttf.getbbox(s)
                im = Image.new("RGBA", (max(box[2], 1), max(box[3], 1)), (0, 0, 0, 0))
                ImageDraw.Draw(im).text((0, 0), s, font=ttf, fill=rgba(fnt["colour2"]))
                return im
        glyphs = self.a.glyphs(fnt["glyph_picture"]) if fnt and fnt.get("glyph_picture") else None
        if not glyphs:
            # No usable font: a plain fallback so the caption is still visible.
            im = Image.new("RGBA", (max(6 * len(s), 1), 11), (0, 0, 0, 0))
            ImageDraw.Draw(im).text((0, 0), s, fill=(40, 40, 40, 255))
            return im
        gs = [glyphs.get(ord(c) if ord(c) < 256 else 0x3f) for c in s]
        w = sum(g.width for g in gs)
        h = max(g.height for g in gs)
        im = Image.new("RGBA", (max(w, 1), max(h, 1)), (0, 0, 0, 0))
        x = 0
        for g in gs:
            im.alpha_composite(g, (x, 0)); x += g.width
        return im

    def textpanel(self, img, rect, item, frame=0):
        self.panel(img, rect, item["panel"], frame)
        self.text(img, rect, item["text"], (item["marginLeft"], item["marginRight"], item["marginTop"], item["marginBottom"]))

    def render(self, fid, visited=()):
        """Composite one form (and its SubForms, recursively) into an RGBA image; returns (img, hotspots)."""
        from PIL import Image, ImageDraw
        form = self.forms.get(fid)
        if form is None:
            return None, []
        W, H = max(form["width"], 1), max(form["height"], 1)
        img = Image.new("RGBA", (W, H), (0, 0, 0, 0))
        self.panel(img, (0, 0, W, H), form["background"])
        hot = []
        order = sorted(range(len(form["controls"])), key=lambda i: form["controls"][i]["base"]["layer"])
        for i in order:
            c = form["controls"][i]
            b, cls, d = c["base"], c["_class"], c["data"]
            rect = (b["x1"], b["y1"], b["x2"], b["y2"])
            info = ""
            if cls == "SubForm":
                sid = d["formId"]
                info = f" -> FRM {sid}"
                if sid and sid not in visited and self.forms.get(sid) is not None:
                    sub, subhot = self.render(sid, visited + (fid,))
                    blit(img, sub, rect[0], rect[1])
                    hot += [(hx + rect[0], hy + rect[1], hx2 + rect[0], hy2 + rect[1], t) for hx, hy, hx2, hy2, t in subhot]
            elif cls in ("Label", "ValueEdit", "TextEdit"):
                self.textpanel(img, rect, d["item"], d.get("frame", 0))
                info = f" text={d['item']['text']['text']!r} pic={d['item']['panel']['picture']['id']}"
            elif cls in ("Switch", "ButtonMenu"):
                sw = d if cls == "Switch" else d["switch"]
                self.textpanel(img, rect, sw["item"], sw["value"])
                info = f" text={sw['item']['text']['text']!r} pic={sw['item']['panel']['picture']['id']} mode={sw['mode']}"
            elif cls == "Selector":
                self.panel(img, rect, d["item"])
                pid = d["picture"]["id"]
                blit(img, self.a.frame(pid, 0), rect[0], rect[1])
                info = f" pic={pid} range={d['min']}..{d['max']} steps={d['steps']}"
            elif cls == "Scope":
                self.panel(img, rect, d["item"])
                ImageDraw.Draw(img, "RGBA").rectangle((rect[0], rect[1], rect[2] - 1, rect[3] - 1), outline=rgba(d["lineColour"]))
            elif cls == "LevelMonitor":
                blit(img, self.a.frame(d["picture"]["id"], 0), rect[0], rect[1])
                info = f" pic={d['picture']['id']}"
            elif cls == "ShadeArea":
                fill = rgba(d["colour"])
                if fill[3]:
                    ImageDraw.Draw(img, "RGBA").rectangle((rect[0], rect[1], rect[2] - 1, rect[3] - 1), fill=fill)
            elif cls in ("List2", "Tree"):
                body = d["body"] if cls == "List2" else d
                if "panel" in body:
                    self.panel(img, rect, body["panel"])
                if "headerItem" in body:
                    self.text(img, rect, body["headerItem"]["text"])
            if self.wire:
                col = CLASS_COLOURS.get(cls, (0, 0, 0))
                ImageDraw.Draw(img, "RGBA").rectangle((rect[0], rect[1], max(rect[2] - 1, rect[0]), max(rect[3] - 1, rect[1])), outline=col + (200,))
            nm = f"/{c['name']}" if c["name"] else ""
            hot.append(rect + (f"FRM {fid} #{c['id']} {c['class']}{nm} {rect} layer={b['layer']}{info}",))
        return img, hot


def preview(rsrc=None, out=None, xmldir=None, wire=False):
    rsrc = rsrc or os.path.join(GUI_DIR, "rsrc")
    out = out or os.path.join(GUI_DIR, "preview")
    os.makedirs(out, exist_ok=True)
    forms = Forms(rsrc, xmldir)
    r = Renderer(forms, Assets(rsrc), wire)
    parts = []
    ids = forms.ids()
    for fid in ids:
        img, hot = r.render(fid)
        if img is None:
            continue
        img.save(os.path.join(out, f"{fid}.png"))
        buf = io.BytesIO(); img.save(buf, "PNG")
        b64 = base64.b64encode(buf.getvalue()).decode()
        spans = "".join(f'<a class="hs" style="left:{x1}px;top:{y1}px;width:{x2-x1}px;height:{y2-y1}px" title="{t}"></a>'
                        for x1, y1, x2, y2, t in hot)
        parts.append(f'<section id="f{fid}"><h2>FRM {fid} ({img.width}x{img.height}, {len(hot)} controls)</h2>'
                     f'<div class="stage" style="width:{img.width}px;height:{img.height}px">'
                     f'<img src="data:image/png;base64,{b64}">{spans}</div></section>')
    nav = " ".join(f'<a href="#f{fid}">{fid}</a>' for fid in ids)
    html = ("<!doctype html><meta charset=utf-8><title>FM8 forms</title><style>body{font:13px system-ui;margin:16px}"
            ".stage{position:relative;border:1px solid #888;background:#fff url('data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAABAAAAAQCAIAAACQkWg2AAAAJklEQVR4nGP4//8/AwMDAwMDA8PAwPDgwYP/Q1UBQ1MBw6ADAAB0Gx3FT9v/AAAAAElFTkSuQmCC')}"
            ".stage img{display:block}.hs{position:absolute;display:block}.hs:hover{outline:2px solid #f0f;background:rgba(255,0,255,.15)}"
            "nav a{margin-right:6px}h2{margin:24px 0 6px}</style>"
            f"<h1>FM8 GUI forms</h1><p>Hover a control for its class, id, rect, text and picture. <a href='pictures.html'>Picture gallery</a>.</p><nav>{nav}</nav>" + "".join(parts))
    with open(os.path.join(out, "index.html"), "w", encoding="utf-8") as f:
        f.write(html)
    print(f"wrote {len(ids)} PNGs and index.html to {out}")


# --- patch / embed -------------------------------------------------------------------------------

def patch(pe_in, pe_out, fid, blob_path, rtype="FRM"):
    """Write one resource into a COPY of a PE with the Win32 UpdateResource API (never the stock file)."""
    import ctypes as C
    import shutil
    shutil.copyfile(pe_in, pe_out)
    k32 = C.WinDLL("kernel32", use_last_error=True)
    k32.BeginUpdateResourceW.restype = C.c_void_p
    k32.BeginUpdateResourceW.argtypes = [C.c_wchar_p, C.c_int]
    k32.UpdateResourceW.argtypes = [C.c_void_p, C.c_wchar_p, C.c_wchar_p, C.c_ushort, C.c_void_p, C.c_uint]
    k32.EndUpdateResourceW.argtypes = [C.c_void_p, C.c_int]
    data = open(blob_path, "rb").read()
    h = k32.BeginUpdateResourceW(pe_out, 0)
    assert h, f"BeginUpdateResource failed: {C.get_last_error()}"
    name = C.cast(C.c_void_p(int(fid) & 0xFFFF), C.c_wchar_p)   # MAKEINTRESOURCE
    ok = k32.UpdateResourceW(h, rtype, name, 1033, data, len(data)) and k32.EndUpdateResourceW(h, 0)
    assert ok, f"UpdateResource failed: {C.get_last_error()}"
    print(f"wrote {rtype} {fid} ({len(data)} bytes) into {pe_out}")


def embed(src, out_h, name="kFrm"):
    """XML (or raw .bin) -> C header with a static byte array, for Rsrc::overrideForm in the shims."""
    data = open(src, "rb").read() if src.endswith(".bin") else build(from_xml(open(src, encoding="utf-8").read()))
    rows = [", ".join(f"0x{b:02x}" for b in data[i:i + 16]) for i in range(0, len(data), 16)]
    with open(out_h, "w", encoding="utf-8") as f:
        f.write(f"// Generated by tools/fm8gui.py embed from {os.path.basename(src)}; do not edit.\n"
                f"#pragma once\nstatic const unsigned char {name}[{len(data)}] = {{\n    " + ",\n    ".join(rows) + "\n};\n")
    print(f"wrote {out_h} ({len(data)} bytes as {name})")


def main():
    a = sys.argv[1:]
    xmldir, wire = None, False
    if "--xml" in a:
        i = a.index("--xml"); xmldir = a[i + 1]; del a[i:i + 2]
    if "--wire" in a:
        a.remove("--wire"); wire = True
    cmd = a[0] if a else "check"
    if cmd == "extract":
        extract(*a[1:3])
    elif cmd == "dump":
        dump(*a[1:3])
    elif cmd == "build":
        open(a[2], "wb").write(build(from_xml(open(a[1], encoding="utf-8").read())))
        print(f"wrote {a[2]}")
    elif cmd == "check":
        sys.exit(0 if check(*a[1:2]) else 1)
    elif cmd == "preview":
        preview(*a[1:3], xmldir=xmldir, wire=wire)
    elif cmd == "pictures":
        gallery(*a[1:3])
    elif cmd == "patch":
        patch(a[1], a[2], a[3], a[4])
    elif cmd == "embed":
        embed(a[1], a[2], *a[3:4])
    else:
        print(__doc__)


if __name__ == "__main__":
    main()
