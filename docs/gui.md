# FM8 GUI: how it is built, and the FM8.plus layout tooling

Hub document. The byte grammar is in [gui-frm-grammar.md](gui-frm-grammar.md), the picture/font/map
resources in [gui-resources.md](gui-resources.md), the runtime (form classes, events, drawing,
how to add a native control) in [gui-runtime.md](gui-runtime.md). The tools are `tools/fm8gui.py`
(decompile, recompile, preview, patch, embed) and `tools/logo_preview.py` (render FM8.plus's own
wordmark edit). Everything below was verified against the real binaries: the XML round-trips all 74
forms byte-exact, the previewer reconstructs the editor from the form data alone, and the widened
header form is what the shipped shims serve to FM8.

## 1. The GUI is data, not code

FM8's GUI runs on Native Instruments' NGL toolkit (`NI::NGL::*`, source path
`nilibs\ngl\core\src\init_ngl.cpp`). Every window layout is a **form**: a serialized list of
**controls** (base class name, id, optional app subclass name, rectangle, typed class-specific
properties) stored as a custom `FRM` resource in the PE `.rsrc` section. The C++ `Form` subclasses
(`FormMain`, `FormArp`, `FormOperator`, ...) do not position anything; they load a FRM by id, look
controls up by id, and bind them to parameters through `ParameterLink` listeners. Drawing is
software into a 32-bit DIB; every picture is a PNG or TGA resource, every caption a glyph blit from
a picture-font strip.

All three binaries (FM8.exe, FM8.dll, FM8.vst3) carry the same GUI code and the same resources.
The analysis is done on FM8.exe (image base 0x140000000); gui-runtime.md section 7 maps ~70
functions to their byte-identical FM8.dll counterparts.

## 2. Resource inventory (FM8.exe `.rsrc`)

| Type | Count | Bytes | What |
|------|------:|------:|------|
| `FRM` | 74 | 341 KB | Form layouts |
| `PICTURE` | 328 | 2.0 MB | Artwork: 262 PNG, 66 Truevision TGA (63 uncompressed, 3 RLE). Pixels are `0xAARRGGBB` in memory |
| `PM` | 5 | 12 KB | PictureManager manifest: per picture `frames`, strip orientation, alpha flag, 9-slice bands |
| `FNT` | 46 | 2.2 KB | Font descriptors: picture fonts (a 256-glyph strip with a marker row) or TrueType (TTFD id + pixel size) |
| `TTFD` | 3 | 6.9 MB | Raw TrueType files (only id 2 is used, by the setup dialogs) |
| `FM` | 5 | 408 B | FontManager manifest (pre-registered font ids per module) |
| `RT_ICON`/`RT_GROUP_ICON`/`RT_VERSION`/`RT_MANIFEST` | | | Ordinary Windows resources |

Resource ids are used directly as Win32 ids (no offsets). The five manifest ids are the modules that
own id ranges: 1 = FM8 (FRM 1..61), 5000 = audio/MIDI setup dialogs (`NI::AB`), 6000 = sound
browser (`NI::SND`), 8000 = activation dialogs, 20000 = the Kore player toolbar; 10000/10001 = About.
The "cannot resolve resource: resources_ENG" log lines are a missing `LOCA` string table, harmless.

## 3. Control classes

The class registry in `init_ngl.cpp` (`FUN_1407374e0`) maps a name to a creator; a record is created
by its `name` if that is a registered class, else by its `class`. Legacy names ending in `.dll` are
aliases. Uses across the 74 forms:

| FRM class | C++ class | Uses | Role and key fields |
|-----------|-----------|-----:|------|
| `Label` | `LabelControl` | 1173 | Caption and/or picture panel: `item` (panel mode/colour/picture, text, font, alignment, margins) |
| `Switch` / `switch.dll` | `SwitchControl` | 476 | Buttons, toggles, tabs, picture buttons (the FM8 wordmark is a Switch on picture 193): `item`, `value`, `toggle`, hover/pressed/disabled frame flags, `mode` |
| `ValueEdit` | `ValueEditControl` | 291 | Numeric fields: typed `min`/`max`, printf `format`, drag sensitivity, `editable` |
| `Selector` / `selector.dll` | `SelectorControl` | 193 | Knobs and sliders: picture strip, `min`/`max`/`steps`, `dragMode` (linear/absolute/rotary), `defaultValue` |
| `SubForm` | `SubFormControl` | 59 | Embeds FRM `formId` at its rect (0 = filled by code at runtime) |
| `TextEdit` | `TextEditControl` | 55 | Editable text: `item`, `multiline`, `readOnly` |
| `ButtonMenu` | `ButtonMenuControl` | 26 | A Switch plus an embedded `PopupMenu` with its item list (label, value) |
| `Scope` / `scope.dll` | `ScopeControl` | 19 | Display: two line colours, `drawMode`, background |
| `LevelMonitor` / `levelMonitor.dll` | `LevelMonitorControl` | 14 | Meter: picture strip, `maxValue`, `peakFrames` |
| `List2`, `List` | `ListControl2` | 13 | Lists: header/row styles, selection colour, scrollbar pane |
| `Generic` | `Control` | 8 | Owner-drawn area (`XYHandle`, `XYHandleMorph`, `EffectList` subclasses) |
| `ShadeArea` | `ShadeAreaControl` | 1 | Dimming overlay colour |
| `Tree View` | `TreeControl` | 1 | Browser tree |

Every FM8-specific control named in a record (`FM8ValueEdit`, `XYHandleMorph`, `MorphSelector`,
`EffectRack`, `SND::ProgramListControl`, ...) keeps its base class's serialization.

## 4. The form tree

FRM 3 (948x562) is the designer's composite of the editor: header FRM 5 at (0,0), navigator FRM 61 at
(0,89), page area FRM 1 at (125,89), keyboard FRM 4 at (0,446). No code loads FRM 3; at runtime
`FM8FormManager` holds every page form as a C++ member, loads each by hard-coded id and positions it
itself (`FUN_14012b830`), producing the same layout. FRM id to class (gui-runtime.md 1.5):

| FRM | Class | What |
|----:|-------|------|
| 5 | `FormMain` | header bar (wordmark, File/Save, sound name, ARP, EDIT ALL, poly, CPU, meter) |
| 15 / 20001 | `FormMainCompact` | compact header |
| 61 | `FormPageSelectNew` | navigator |
| 1 | `FormEasy` | Easy/Morph page |
| 4 | `FormKeyboard` | keyboard strip (FRM 25 is the 4px bar shown when the keyboard is hidden) |
| 6 | `FormArp` | Arpeggiator |
| 27 | `FormEffects` | Effects page (SubForm 0 is filled with the effect rack; strips are FRM 38..50 inside racks 2/9/10) |
| 28 | `FormOperatorA2F` | Operators A..F (one form, operator index switched in code) |
| 29 / 30 / 31 | `FormPitch` / `FormFilter` / `FormSat` | Pitch, Filter (Z), Saturator (X) |
| 32 | `FormDeepFreq` | Expert: Ops (FM matrix built by cloning FRM 7's controls 11x11) |
| 33 / 34 | `FormDeepEnv` / `FormDeepEnv2` | Expert: Env |
| 36 | `FormModMatrix` | Expert: Mod (18 copies of FRM 8 columns) |
| 37 | `FormDeepSpec` | Expert: Spectrum |
| 57 | `FormFX` | Master |
| 58 / 59 | `SoundBrowserForm` / `SoundAttributesForm` | Browser / Attributes (SubForm 0 filled by the `NI::SND` browser, FRM 6000..6007) |
| 16, 22, 23, 25 | `NGL::Form` | plain coloured bars |
| 11/12/13, 5000/5001/5008, 8000/8001, 10000/10001 | dialogs | options, audio/MIDI setup, activation, about |

Page switching is `FUN_14012fda0(pageId)`: hide the current page form, show the new one.

## 5. FRM format and the XML

Little-endian; strings are `u32 length` + bytes. Full field tables and version branches are in
gui-frm-grammar.md; the shipped forms use the newest version of every record type.

```
Form:     u32 version=6 | PanelItem background | i32 width | i32 height | u8 resizable [| i32 minW, minH]
          | str name | i32 count | count x control
control:  u32 marker=1 | u32 id | str class | str name
          | ControlBase: u32 version=7 | i32 x1 y1 x2 y2 | u8 transparent | u32 tag | u32 layer | str help
          | class-specific fields (own version first), e.g. Switch: pressed offsets, TextPanelItem, value, flags, mode ...
PanelItem: u32 1 | u32 mode (0 none, 1 colour, 2 picture frame, 3 picture clamped) | rgba colour | ResourceRef picture
TextItem:  u32 version | ResourceRef font | u32 hAlign | u32 vAlign | str text
```

The XML mirrors that grammar one to one, so it is lossless: primitives are attributes, nested items
are child elements named after the field, the class data is one child element named after the
class. Colours are `AARRGGBB` hex. `count` fields are recomputed on build, so controls and menu
items can be added or removed freely.

```xml
<form version="6" width="948" height="89" resizable="0" name="" id="5">
  <background version="1" mode="2" colour="ffb0b0b0"><picture version="1" id="113"/></background>
  <control id="5" class="switch.dll" rect="21,35,116,58" layer="3">
    <Switch version="11" pressedOffsetX="0" pressedOffsetY="0" value="0" toggle="0" hoverFrames="1" ... mode="0" ...>
      <item version="2" marginLeft="0" marginRight="0" marginTop="0" marginBottom="0">
        <panel version="1" mode="2" colour="ffb0b0b0"><picture version="1" id="193"/></panel>
        <text version="2" hAlign="0" vAlign="0" text=""><font version="1" id="0"/></text>
      </item>
    </Switch>
  </control>
```

## 6. Tooling

```
python tools/fm8gui.py extract [pe] [outdir]       pull FRM/PICTURE/FNT/FM/PM/TTFD out of a PE (PNGs saved as .png)
python tools/fm8gui.py dump    [rsrcdir] [xmldir]  decompile every FRM to XML
python tools/fm8gui.py build   <xml> <out.bin>     recompile one XML to a FRM blob
python tools/fm8gui.py check   [rsrcdir]           prove parse -> XML -> build is byte-exact for every form
python tools/fm8gui.py preview [rsrcdir] [outdir]  render every form (SubForms composited, real artwork and captions)
                               --xml DIR           render from your edited XML instead of the binaries
                               --wire              add class-coloured outlines
python tools/fm8gui.py pictures [rsrcdir] [outdir] contact sheet of every PICTURE with frames/orientation/9-slice bands
python tools/fm8gui.py patch   <pe-in> <pe-out> <id> <frm.bin>   write a FRM into a COPY of a PE (test builds)
python tools/fm8gui.py embed   <xml|bin> <out.h> [name]        C header for the runtime override
python tools/logo_preview.py [pe] [outdir]         render FM8.plus's widened "FM8+" wordmark, header bar and editor
```

Defaults live under `build/gui/` (`rsrc/`, `xml/`, `preview/`, `logo-preview/`), all git-ignored:
the forms and artwork are Native Instruments' data and are never checked in, and nothing under
`build/gui/` is compiled into FM8.plus either. `preview/index.html` shows
each form with hover hotspots (form, id, class/subclass, rect, layer, text, picture); the renderer
reproduces the editor closely enough to judge a layout change without launching FM8 (`build/gui/
preview/3.png` is the whole editor). `tools/frm_grammar.py` holds the grammar table and the generic
parser, serializer and XML mapping; `fm8gui.py` never hard-codes a field.

## 7. Runtime: serving rebuilt resources, and the "FM8+" button

FM8 loads every form and picture with `FindResourceA(hModule, id, type)` + `LoadResource` +
`LockResource` on its own module. `src/core/rsrc.cpp` patches those four entries in the FM8 module's
**own import table** (FM8.exe, FM8.dll or FM8.vst3, whichever the shim loaded) and answers registered
(type, id) pairs from static blobs; the host DAW's imports and the stock file are untouched.
`Core::serveLogo(module)` registers three, each **built at runtime from the module's own stock
resource**, so no Native Instruments form data or artwork is compiled into FM8.plus at all:

| Resource | What FM8.plus changes | How |
|---|---|---|
| `FRM` 5 | FormMain header: the wordmark control's rect from (21,35,116,58) to (10,35,127,58) | copy the blob, rewrite the one 16-byte rect pattern (unique in the form, and only forms 5 and 15 contain it, checked against all 74) |
| `FRM` 15 | FormMainCompact: the same control | as above |
| `PICTURE` 193 | the wordmark bitmap, 95x23 to 117x23, with the "+" in the new space | decode the stock PNG with GDI+, widen, rasterize the cross with 4x4 coverage sampling, emit an uncompressed 32-bit TGA (`Picture::Load` accepts PNG, JPEG or TGA, and a TGA is a header in front of the BGRA bytes it already wants) |

Every shim calls it before FM8 builds its GUI. The bitmap itself is built the first time FM8 asks for
it rather than up front, because it needs GDI+ and the standalone registers everything from `DllMain`,
where loading a DLL would take the loader lock. From there the "+" is FM8's own artwork: FM8 draws it,
clips it to the control, and GUI Scale stretches it with everything else, so there is no window of
ours over the editor and nothing to lose a Z-order fight with the host.

The click is all that is left. `src/core/ui.cpp` subclasses the window FM8 draws the editor into (the
`NIVSTChildWindow` the shim finds under the host's editor HWND, or FM8's own top-level window in the
standalone) and opens the FM8.plus menu on a left press inside the wordmark rect, swallowing it so
FM8's own logo Switch never sees it. Everything else passes through untouched. `SetWindowSubclass`
only takes effect on the window's own thread, so the standalone, which finds the window from a worker
thread, rides into FM8's UI thread on a one-shot `WH_CALLWNDPROC` hook.

Swallowing the press costs what stock FM8 does with it, which is open the About panel, so the menu
gives it back: **About FM8** calls FM8's own dialog function with FM8's own `FM8App` pointer
(docs/hooks.md, "The logo click and the About panel"), and **About FM8.plus** opens the project page.
Taking the click at the Win32 layer rather than detouring the command sink keeps the menu bound to
the right instance (the subclass belongs to one editor window, the sink is shared by every FM8 in the
process) and keeps the popup out of the Switch's own mouse capture.

`tools/vsteditor.py` checks both halves end to end in a real editor: which resource path is live
(`resource hook: FM8.dll!FindResourceA import -> FM8.plus.dll`), that the "+" pixels are on the
wordmark, and that a posted click there opens the menu.

Workflow for a GUI change:

1. `python tools/fm8gui.py extract && python tools/fm8gui.py dump`, edit `build/gui/xml/<id>.xml`
   (move a rect, add a `<control>`, add a menu item, point a `SubForm` at a new form id you author).
2. `python tools/fm8gui.py preview --xml build/gui/xml` and look at `build/gui/preview/<id>.png`.
3. Either implement it in `src/core/rsrc.cpp` (which edits the served blob at runtime) and rebuild,
   or `patch` it into a scratch copy of FM8.exe to test with the real engine.
   `python tools/logo_preview.py` renders the wordmark edit the shipped code makes.
4. For new interactive controls, gui-runtime.md section 5 gives the native recipe (create by class
   name, set rect/id/picture, add a listener, `Form::addControl`) and the notification codes; a
   control added to a form via XML draws and hit-tests like any stock control, and its clicks reach
   the form's listener with its id.
