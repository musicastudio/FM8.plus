"""Table-driven grammar of FM8's FRM form resources (docs/gui-frm-grammar.md), with generic
parse/serialize and XML conversion. The tables are data; nothing here knows what a Switch is.

Types: i32 u32 u8 f32 f64 str rgba | <Struct> | if:<field>:<Struct> | ifzero:<field>:<Struct> |
list:<field>:<Struct> | switch:<field>:<Struct> (variant table) | controls (count control records).
"""
from __future__ import annotations

import collections
import struct
import xml.etree.ElementTree as ET

GRAMMAR = {
    "Form": [["version", "u32"], ["background", "PanelItem"], ["width", "i32"], ["height", "i32"],
             ["resizable", "u8"], ["minSize", "if:resizable:MinSize"], ["name", "str"], ["count", "i32"],
             ["controls", "controls"]],
    "MinSize": [["minWidth", "i32"], ["minHeight", "i32"]],
    "ControlHeader": [["marker", "u32"], ["id", "u32"], ["class", "str"], ["name", "str"]],
    "ControlBase": [["version", "u32"], ["x1", "i32"], ["y1", "i32"], ["x2", "i32"], ["y2", "i32"],
                    ["transparent", "u8"], ["tag", "u32"], ["layer", "u32"], ["help", "str"]],
    "ResourceRef": [["version", "u32"], ["id", "i32"]],
    "PanelItem": [["version", "u32"], ["mode", "u32"], ["colour", "rgba"], ["picture", "ResourceRef"]],
    "TextItem": [["version", "u32"], ["font", "ResourceRef"], ["hAlign", "u32"], ["vAlign", "u32"], ["text", "str"]],
    "TextPanelItem": [["version", "u32"], ["panel", "PanelItem"], ["text", "TextItem"], ["marginLeft", "u32"],
                      ["marginRight", "u32"], ["marginTop", "u32"], ["marginBottom", "u32"]],
    "SelectableTextPanelItem": [["base", "TextPanelItem"], ["version", "u32"], ["selPicture", "ResourceRef"],
                                ["selFont", "ResourceRef"], ["selColour", "rgba"], ["selColour2", "rgba"],
                                ["flag_e8", "u8"], ["flag_e9", "u8"]],
    "ScrollablePane": [["version", "u32"]] + [[f"part{i}", "PanelItem"] for i in range(9)] + [["barWidth", "i32"]]
                      + [[f"i32_{o}", "i32"] for o in ("218", "21c", "220", "208", "20c", "210", "214", "384", "388",
                                                        "38c", "390", "394", "398", "39c", "3a0")]
                      + [[f"mode_{o}", "u32"] for o in ("308", "30c", "310", "314")],
    "MenuItem": [["separator", "u8"], ["entry", "ifzero:separator:MenuEntry"]],
    "MenuEntry": [["label", "str"], ["value", "i32"]],
    "ValueRange_f32": [["min", "f32"], ["max", "f32"]],
    "ValueRange_i32": [["min", "i32"], ["max", "i32"]],
    "ValueRange_f64": [["min", "f64"], ["max", "f64"]],
    "Generic": [],
    "Label": [["version", "u32"], ["item", "TextPanelItem"], ["frame", "u32"]],
    "Switch": [["version", "u32"], ["pressedOffsetX", "i32"], ["pressedOffsetY", "i32"], ["item", "TextPanelItem"],
               ["value", "u32"], ["toggle", "u8"], ["hoverFrames", "u8"], ["pressedFrames", "u8"],
               ["disabledFrame", "u8"], ["primaryButtonOnly", "u8"], ["mode", "u32"], ["repeatInterval", "u32"],
               ["repeatDelay", "u32"], ["keyboard", "u8"], ["text", "str"]],
    "SubForm": [["version", "u32"], ["formName", "str"], ["formId", "u32"]],
    "ValueEdit": [["version", "u32"], ["item", "TextPanelItem"], ["editable", "u8"], ["zeroSpecial", "u8"],
                  ["dragScale", "i32"], ["dragSensitivity", "f32"], ["flag_1ce", "u8"], ["valueType", "u32"],
                  ["range", "switch:valueType:ValueRange"], ["format", "str"], ["flag_1d0", "u8"]],
    "Selector": [["version", "u32"], ["picture", "ResourceRef"], ["item", "PanelItem"], ["min", "f32"], ["max", "f32"],
                 ["steps", "u32"], ["hasPicture", "u8"], ["vertical", "u8"], ["keyboard", "u8"], ["disabledFrame", "u8"],
                 ["dragMode", "u32"], ["sensitivity", "f32"], ["handleMargin", "u32"], ["centreX", "i32"],
                 ["centreY", "i32"], ["flag_150", "u8"], ["fineFactor", "f32"], ["defaultValue", "f32"],
                 ["resetMode", "u32"]],
    "TextEdit": [["version", "u32"], ["item", "TextPanelItem"], ["multiline", "u8"], ["readOnly", "u8"],
                 ["flag_555", "u8"], ["flag_5a8", "u8"], ["disabledFrame", "u8"]],
    "ButtonMenu": [["switch", "Switch"], ["version", "u32"], ["captionFromSelection", "u8"], ["menu", "PopupMenu"]],
    "PopupMenu": [["control", "ControlBase"], ["version", "u32"], ["count", "u32"], ["items", "list:count:MenuItem"]],
    "Scope": [["version", "u32"], ["lineColour", "rgba"], ["lineColour2", "rgba"], ["drawMode", "u32"],
              ["item", "PanelItem"]],
    "LevelMonitor": [["version", "u32"], ["picture", "ResourceRef"], ["maxValue", "u32"], ["peakFrames", "u32"]],
    "ShadeArea": [["version", "u32"], ["colour", "rgba"]],
    "List2": [["body", "switch:version:List2"]],
    "List2_v12": [["version", "u32"], ["headerItem", "TextPanelItem"], ["flag_13da", "u8"],
                  ["rowItem", "SelectableTextPanelItem"], ["flag_13d8", "u8"], ["selectionColour", "rgba"],
                  ["panel", "PanelItem"], ["i32_13c8", "i32"], ["i32_11a0", "i32"], ["i32_13cc", "i32"],
                  ["flag_13d9", "u8"], ["i32_13e0", "i32"], ["i32_13d0", "i32"], ["pictureA", "u32"],
                  ["pictureB", "u32"], ["i32_13e4", "i32"], ["pane", "ScrollablePane"]],
    "List2_v9": [["version", "u32"], ["headerItem", "TextPanelItem"], ["rowItem", "TextPanelItem"],
                 ["i32_13c8", "i32"], ["i32_13cc", "i32"], ["i32_13d0", "i32"], ["i32_13e4", "i32"],
                 ["colour_13c4", "rgba"], ["partsColour", "rgba"], ["colour_1388", "rgba"], ["ignoredA", "u32"],
                 ["ignoredB", "u32"], ["flag_13d8", "u8"], ["selectionColour", "rgba"], ["pictureA", "u32"],
                 ["pictureB", "u32"], ["flag_set270", "u8"], ["flag_1390", "u8"]],
    "Tree": [["version", "u32"], ["flag_659", "u8"], ["flags_480", "i32"], ["indent", "i32"],
             ["indentPerLevel", "i32"], ["resource_470", "u32"], ["resource_5c0", "u32"],
             ["rowItem", "SelectableTextPanelItem"], ["panel", "PanelItem"], ["pane", "ScrollablePane"]],
}

VARIANTS = {
    "ValueRange": {"0": "ValueRange_f32", "1": "ValueRange_i32", "2": "ValueRange_i32", "3": "ValueRange_i32",
                   "4": "ValueRange_f64"},
    "List2": {"9": "List2_v9", "10": "List2_v9", "12": "List2_v12"},
}

# registry name -> grammar class. FM8's own classes (used as the record `name`) keep their base loader.
CLASSES = {
    "Generic": "Generic", "Label": "Label", "Switch": "Switch", "switch.dll": "Switch", "SubForm": "SubForm",
    "StackedSubForm": "SubForm", "ValueEdit": "ValueEdit", "Selector": "Selector", "selector.dll": "Selector",
    "TextEdit": "TextEdit", "ButtonMenu": "ButtonMenu", "Scope": "Scope", "scope.dll": "Scope",
    "LevelMonitor": "LevelMonitor", "levelMonitor.dll": "LevelMonitor", "List2": "List2", "List": "List2",
    "ShadeArea": "ShadeArea", "Shade Area": "ShadeArea", "Tree View": "Tree",
    "FM8ValueEdit": "ValueEdit", "MorphSelector": "SubForm", "Arpeggiator": "SubForm", "EffectRack": "SubForm",
    "EffectList": "Generic", "XYHandle": "Generic", "XYHandleMorph": "Generic", "SoundAttributesSFC": "SubForm",
    "SoundBrowserSFC": "SubForm", "BrowserTree": "Tree", "DelayedTextControl": "TextEdit",
    "AttributesListControl": "List2", "SelectAttributesListControl": "List2", "FixColumnListControl": "List2",
    "ListControl2Helper": "List2", "MIDI_CCList": "List2", "SND::ProgramListControl": "List2",
    "FontChangeSwitch": "Switch", "MoverControl": "Switch", "BrowserMainMover": "SubForm",
    "SND::ProgramListSubForm": "SubForm", "DetailSearchPage": "SubForm", "FM8Envelope": "Generic",
    "ButtonMenuControl2": "ButtonMenu",
}

PRIM = {"u32": "<I", "i32": "<i", "f32": "<f", "f64": "<d", "rgba": "<I"}


def grammar_class(ctl):
    """Grammar class for a control dict (the record name wins over the class, as the loader does)."""
    key = ctl["name"] if ctl["name"] in CLASSES else ctl["class"]
    if key not in CLASSES:
        raise ValueError(f"unknown control class {ctl['class']!r}/{ctl['name']!r}")
    return CLASSES[key]


# --- binary --------------------------------------------------------------------------------------

class _Reader:
    def __init__(self, b):
        self.b, self.p = b, 0

    def prim(self, t):
        if t == "u8":
            v = self.b[self.p]; self.p += 1; return v
        if t == "str":
            n = self.prim("u32")
            s = self.b[self.p:self.p + n].decode("latin-1"); self.p += n; return s
        v = struct.unpack_from(PRIM[t], self.b, self.p)[0]; self.p += struct.calcsize(PRIM[t]); return v


def _enc(t, v):
    if t == "u8":
        return bytes([v])
    if t == "str":
        b = v.encode("latin-1"); return struct.pack("<I", len(b)) + b
    return struct.pack(PRIM[t], v)


def _parse_struct(r, name):
    out = collections.OrderedDict()
    for fname, ftype in GRAMMAR[name]:
        if ftype in PRIM or ftype in ("u8", "str"):
            out[fname] = r.prim(ftype)
        elif ftype == "controls":
            out[fname] = [_parse_control(r) for _ in range(out["count"])]
        elif ftype.startswith(("if:", "ifzero:")):
            kind, field, sub = ftype.split(":")
            present = (out[field] != 0) if kind == "if" else (out[field] == 0)
            out[fname] = _parse_struct(r, sub) if present else None
        elif ftype.startswith("list:"):
            _, field, sub = ftype.split(":")
            out[fname] = [_parse_struct(r, sub) for _ in range(out[field])]
        elif ftype.startswith("switch:"):
            _, field, sub = ftype.split(":")
            key = str(out[field]) if field in out else str(struct.unpack_from("<I", r.b, r.p)[0])
            out[fname] = _parse_struct(r, VARIANTS[sub][key]); out[fname]["_variant"] = VARIANTS[sub][key]
        else:
            out[fname] = _parse_struct(r, ftype)
    return out


def _parse_control(r):
    c = _parse_struct(r, "ControlHeader")
    c["base"] = _parse_struct(r, "ControlBase")
    c["_class"] = grammar_class(c)
    c["data"] = _parse_struct(r, c["_class"])
    return c


def _ser_struct(name, d):
    out = bytearray()
    for fname, ftype in GRAMMAR[name]:
        v = d[fname]
        if ftype in PRIM or ftype in ("u8", "str"):
            out += _enc(ftype, v)
        elif ftype == "controls":
            for c in v:
                out += _ser_struct("ControlHeader", c) + _ser_struct("ControlBase", c["base"]) + _ser_struct(c["_class"], c["data"])
        elif ftype.startswith(("if:", "ifzero:")):
            if v is not None:
                out += _ser_struct(ftype.split(":")[2], v)
        elif ftype.startswith("list:"):
            for it in v:
                out += _ser_struct(ftype.split(":")[2], it)
        elif ftype.startswith("switch:"):
            out += _ser_struct(v["_variant"], v)
        else:
            out += _ser_struct(ftype, v)
    return bytes(out)


def parse(b):
    r = _Reader(b)
    form = _parse_struct(r, "Form")
    if r.p != len(b):
        raise ValueError(f"{len(b) - r.p} trailing bytes")
    return form


def serialize(form):
    form["count"] = len(form["controls"])
    for c in form["controls"]:
        c["_class"] = grammar_class(c)
    for name, d in _walk_counts(form):
        d["count"] = len(d["items"])
    return _ser_struct("Form", form)


def _walk_counts(d):
    """Every PopupMenu dict (has items) so its count can be refreshed before writing."""
    if isinstance(d, dict):
        if "items" in d and "count" in d:
            yield "PopupMenu", d
        for v in d.values():
            yield from _walk_counts(v)
    elif isinstance(d, list):
        for v in d:
            yield from _walk_counts(v)


# --- XML -----------------------------------------------------------------------------------------
# Primitives become attributes, nested structures child elements named after the field, lists repeat
# the child element, a `switch` child carries variant="...". A <control> merges its header and base
# fields (rect="x1,y1,x2,y2") and holds one child element named after the grammar class.

def _fmt(t, v):
    if t == "rgba":
        return f"{v:08x}"
    if t in ("f32", "f64"):
        return repr(float(v))
    return str(v)


def _val(t, s, default=None):
    if s is None:
        return default if default is not None else ("" if t == "str" else 0)
    if t == "rgba":
        return int(s, 16)
    if t in ("f32", "f64"):
        return float(s)
    if t == "str":
        return s
    return int(s)


def _to_elem(name, d, tag):
    el = ET.Element(tag)
    if "_variant" in d:
        el.set("variant", d["_variant"])
    for fname, ftype in GRAMMAR[name]:
        v = d[fname]
        if ftype in PRIM or ftype in ("u8", "str"):
            el.set(fname, _fmt(ftype, v))
        elif ftype.startswith(("if:", "ifzero:")):
            if v is not None:
                el.append(_to_elem(ftype.split(":")[2], v, fname))
        elif ftype.startswith("list:"):
            for it in v:
                el.append(_to_elem(ftype.split(":")[2], it, fname[:-1] if fname.endswith("s") else fname))
        elif ftype.startswith("switch:"):
            el.append(_to_elem(v["_variant"], v, fname))
        elif ftype == "controls":
            pass
        else:
            el.append(_to_elem(ftype, v, fname))
    return el


def _from_elem(name, el):
    d = collections.OrderedDict()
    for fname, ftype in GRAMMAR[name]:
        if ftype in PRIM or ftype in ("u8", "str"):
            d[fname] = _val(ftype, el.get(fname))
        elif ftype.startswith(("if:", "ifzero:")):
            kind, field, sub = ftype.split(":")
            child = el.find(fname)
            present = (d[field] != 0) if kind == "if" else (d[field] == 0)
            d[fname] = _from_elem(sub, child) if (present and child is not None) else None
        elif ftype.startswith("list:"):
            _, field, sub = ftype.split(":")
            tag = fname[:-1] if fname.endswith("s") else fname
            d[fname] = [_from_elem(sub, c) for c in el.findall(tag)]
            d[field] = len(d[fname])
        elif ftype.startswith("switch:"):
            _, field, sub = ftype.split(":")
            child = el.find(fname)
            variant = child.get("variant") or VARIANTS[sub][str(d[field]) if field in d else child.get("version")]
            d[fname] = _from_elem(variant, child); d[fname]["_variant"] = variant
        elif ftype == "controls":
            d[fname] = []
        else:
            d[fname] = _from_elem(ftype, el.find(fname))
    return d


def to_xml(fid, form):
    root = _to_elem("Form", form, "form")
    root.set("id", str(fid))
    del root.attrib["count"]
    for c in form["controls"]:
        el = ET.SubElement(root, "control")
        el.set("id", str(c["id"])); el.set("class", c["class"])
        if c["name"]:
            el.set("name", c["name"])
        if c["marker"] != 1:
            el.set("marker", str(c["marker"]))
        b = c["base"]
        el.set("rect", f"{b['x1']},{b['y1']},{b['x2']},{b['y2']}")
        for k in ("transparent", "tag", "layer", "help"):
            if b[k] not in (0, ""):
                el.set(k, str(b[k]))
        if b["version"] != 7:
            el.set("baseVersion", str(b["version"]))
        el.append(_to_elem(c["_class"], c["data"], c["_class"]))
    ET.indent(root)
    return ET.tostring(root, encoding="unicode")


def from_xml(text):
    root = ET.fromstring(text)
    form = _from_elem("Form", root)
    for el in root.findall("control"):
        c = collections.OrderedDict(marker=int(el.get("marker", "1")), id=int(el.get("id")), **{"class": el.get("class")},
                                    name=el.get("name", ""))
        x1, y1, x2, y2 = map(int, el.get("rect").split(","))
        c["base"] = collections.OrderedDict(version=int(el.get("baseVersion", "7")), x1=x1, y1=y1, x2=x2, y2=y2,
                                            transparent=int(el.get("transparent", "0")), tag=int(el.get("tag", "0")),
                                            layer=int(el.get("layer", "0")), help=el.get("help", ""))
        c["_class"] = grammar_class(c)
        child = el.find(c["_class"])
        c["data"] = _from_elem(c["_class"], child) if child is not None else _from_elem(c["_class"], ET.Element(c["_class"]))
        form["controls"].append(c)
    return form
