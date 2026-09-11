# FM8 GUI runtime (NGL forms, controls, events, drawing)

How FM8 (build 2022-12-23) builds and runs its GUI, reverse-engineered from the Ghidra
decompilation of FM8.exe (base `0x140000000`). The NGL/UIA code is byte-identical in
FM8.dll (VST2, base `0x180000000`); vst2 addresses were located by normalized-body matching
plus string anchors and are listed in section 7. Everything not marked UNCONFIRMED was read
in the decompiled C. Query any address with `python tools/q.py exe fn 0x...`.

Vocabulary: "slot N / +0xNN" is a vtable entry (slot*8). `VT(o)` is `*(void***)o`. Sizes are
bytes. Offsets are from the object start unless a sub-object is named.

## 0. Architecture in one paragraph

NI::NGL is a retained-mode toolkit. A `FormManager` (a `FormContainer`) owns up to 128 `Form`
slots; each `Form` owns an intrusive list of `Control`s. FM8 subclasses the manager as
`FM8FormManager` (size `0x10558`) and embeds every page form as a C++ member; there is no
FRM-id-to-class table. Each member form is loaded by id through `FormContainer::loadForm(form,
frmId, show)`, which calls `Form::load(frmId)`; that asks the `ResourceManager` for an `FRM`
resource from the module's own `.rsrc`, parses the header, and for each control record creates
the control by class name from a global registry, streams it in, sets its id and appends it to
the form. Mouse input arrives through a Win32 window (`NI::UIA` layer), is translated into NGL
event ids, hit-tested down manager -> form -> control, and a control turns a click into
`notify(type, data)` on its listener vector; FM8's `ParameterLink` objects are those listeners
and write the engine through `FM8EditBuffer::setParameterByTag`. Drawing is software: forms
paint controls into a 32-bit DIB through `NI::UIA::Graphics`, flushed with
`SetDIBitsToDevice`/`StretchDIBits` on `WM_PAINT`. No OpenGL or Direct2D on this path.

## 1. Load path

### 1.1 Resource access

| Function | exe | Role |
|---|---|---|
| `FUN_1408f85f0` | `0x1408f85f0` | `FindResourceA(hModule, MAKEINTRESOURCE(id & 0xffff), typeName)` + `LoadResource`/`LockResource`, wraps bytes into a memory stream at `stream+0x18`. Falls back to `FUN_1408f8cf0` (file on disk, dev builds). |
| `FUN_1408db5a0` | `0x1408db5a0` | Allocates the 0x38-byte stream object, gets `hModule = *FUN_140906750()` and calls the above. `hModule` is the HINSTANCE of the module hosting NGL (FM8.exe, or FM8.dll/FM8.vst3 for the plug-ins; recorded by the `NI::UIA::AppModule` singleton `DAT_141203f40`, ctor `FUN_1408dad90`). |
| `FUN_1408db250` | `0x1408db250` | Thunk `(unused, id, typeName, 1)` used by every resource type loader. |
| `NI::NGL::ResourceManager::vftable` | `0x140d4de20` | slot 1 (+0x08) `FUN_140766230(this, uint id, const char* type)` = `loadResource`. Compares the 4-byte type against `DAT_140aaf7c8` (the `FRM` tag; `FUN_140765eb0` returns it), consults a localisation map at `+0x20` (id -> name -> per-language id, language from `FUN_1408f62e0`), then calls `FUN_1408db250(hmod, id, type, 1)`. `+0x10` = `PictureManager*` (used as `*(Form+0x78)+0x10`), `+0x30` = control-id remap map (see 1.3). |

Other type loaders on the same thunk: `FUN_140764210` ("PICTURE"), `FUN_140763fb0` (fonts,
`font_ngl.cpp`), `FUN_140763e70` (raw blobs), `FUN_140763740`/`FUN_140766880`/`FUN_140766d70`
(generic `Resource::load(id)`: asks the object for its type string via slot +0x50 and streams
via slot +0x30).

### 1.2 Form::load and the FRM parser

`Form::load(int frmId)` = Form slot 5 (+0x28) = `FUN_140754bc0`:

```c
this+0xc = -1;
VT(this)[27](this);                 // +0xd8 removeAllControls (FUN_14075e3b0, role inferred from call sites)
VT(this)[17](this);                 // +0x88 unload
res = VT(rm)[1](rm, frmId, "FRM");  // rm = *(Form+0x78) ResourceManager
if (res) { this+0xc = frmId; VT(this)[38](this,res) /*+0x130 readFromResource*/; release(res); VT(this)[16](this) /*+0x80 init*/; }
```

`Form::readFromResource(stream)` = slot 38 (+0x130) = `FUN_14075d6a0` (asserts
`"Invalid Form resource version!"`, `form_ngl.cpp:0x840`, version <= 6): v1 reads a background
picture id; else the background `PanelItem` at `Form+0x48` reads itself (`VT(+0x48)[1]`);
v>2 width/height -> `+0x30/+0x34`; v>4 resizable byte `+0x71` plus min size; v>5 form name
-> `+0x10` (std::string). Then `count = u32` and `count` times `FUN_14075dcf0(form, stream,
version<4)`:

```c
u32 ctrlVersion; u32 ctrlId; string className; string subclassName;   // stream order
ctrl = FUN_14072c990(subclassName) ?: FUN_14072c990(className);          // registry create
if (ctrl && VT(ctrl)[31](ctrl, stream) /*+0xf8 readFromStream*/) {
    id = FUN_140765ec0(rm, form+0xc, ctrlId);   // localisation remap; 0 drops the control
    if (id) { VT(ctrl)[5](ctrl, id) /*+0x28 setId*/; VT(form)[23](form, ctrl) /*+0xb8 addControl*/; }
}
```

`Control::readFromStream` = Control slot 31 = `FUN_140746030` (`control_ngl.cpp:0x248`,
`"Invalid Control resource version!"`, version <= 7): v>=6 reads rect (4 x u32, applied via slot
8 `setRect`), byte -> `+0x84`, u32 -> `+0x88`, u32 layer -> `+0x80`, v>=7 string -> `+0x60`.
Subclasses chain to it first (Switch `FUN_140747510`, `switchcontrol_ngl.cpp:0x26d`,
version <= 11; SubForm `FUN_1407473d0`, `subformcontrol_ngl.cpp:400`, version <= 1).

So rects in the FRM are final for stock controls; the only procedural placement in FM8 is the
FM matrix (`FUN_14012acb0`, which loads FRM 7 as a template, clones its controls 11 x 11 with a
0x1c pitch and appends them to the matrix form) and the page-form positioning in 2.5.

### 1.3 Control registry

Global vector `DAT_141201728..DAT_141201730` of 0x28-byte entries `{std::string name (0x20);
Control* (*creator)() (+0x20)}`. Lookup `FUN_14072c990(const char* name)` does a linear
`memcmp`, calls the creator, and copies the registry name into `Control+0x08` (so
`Control+0x08` is the class-name string; FM8 code checks it, e.g. `FUN_1400d1a60`). Registration
`FUN_14072a0d0(name, creator)` (or `FUN_14072a0b0(nameGetter, creator)`); the NGL init
`FUN_1407374e0` (`init_ngl.cpp`) registers:

| Name | Creator | Object size / class |
|---|---|---|
| `Generic` | `FUN_14072c030` | base Control |
| `Label` | `FUN_14072c080` | |
| `LevelMonitor`, `levelMonitor.dll` | `FUN_14072c180` | |
| `List2` | `FUN_14072c210` | |
| `ObjectBoxControl` | `FUN_14072c300` | |
| `Scope`, `scope.dll` | `FUN_14072c340` | |
| `Selector`, `selector.dll` | `FUN_14072c3f0` | SelectorControl |
| `ShadeArea`, `Shade Area` | `FUN_14072c560` | |
| `StackedSubForm` | `FUN_14072c5f0` | StackedSubFormControl (vt `0x140d49d60`) |
| `SubForm` | `FUN_14072c650` | SubFormControl (vt `0x140d462e8`), size 0x1140 |
| `Switch`, `switch.dll` | `FUN_14072c690` | SwitchControl (vt `0x140d461a8`), size 0x1d8 |
| `TextEdit` | `FUN_14072c7d0` | |
| `Tree View` | `FUN_14072c810` | |
| `ValueEdit` | `FUN_14072c850` | ValueEditControl (vt `0x140d46c38`) |
| `ButtonMenu` | `FUN_140754d70` via `FUN_140755540` | ButtonMenuControl (vt `0x140d4c658`), size 0x3c8, derives SwitchControl, PopupMenu embedded at +0x1e0 |
| `ButtonMenuControl2` | `FUN_140754e40` via `FUN_140755560` | 0x2c58, different class (EasyPopupMenuControl inside) |
| `FM8Envelope` | `FUN_1400bf880` via `FUN_1400c0f10` | FM8's own |
| `BrowserMainMover`, `DetailSearchPage` | `FUN_14030f1a0`, `FUN_140308590` | browser |

`FM8ValueEditControl` (vt `0x140a814c0`) is not registered by name; where FM8 instantiates it
is UNCONFIRMED (not from FRM records).

### 1.4 SubForm control -> child form

`SubFormControl` layout (base Control 0xc8 + embedded `FormContainer` at `+0xc8` (0x1040) +
fields): `+0x1108` int child FRM id, `+0x1110` `Form*` child, `+0x1118` std::string child form
name, `+0x1138` byte registered-with-parent. Read from stream (`FUN_1407473d0`): name then
`+0x1108`.

Attach = SubFormControl slot 23 (+0xb8) = `FUN_140740c50(this, Form* parent)`:

```c
this+0xa8 = parent;
if (!parent || !*(parent+0x80)) FormContainer::ctor(this+0xc8) else VT(this+0xc8)[1]();   // reset container
if (*(parent+0x78)) {                                             // parent has a ResourceManager
  if (!this+0x1110) {
    child = VT(this)[39](this);            // +0x138 createSubForm: FUN_140739630 looks the name (+0x1118) up via FUN_140754fa0, else new NGL::Form (0x248)
    this+0x1110 = child;
    VT(child)[14](child, *(parent+0x78));  // +0x70 setResourceManager
    if (this+0x1108 == 0) { VT(child)[9](child,&sizeFromRect); VT(this+0xc8)[4](this+0xc8, child, 1); }   // addForm
    else                  { VT(this+0xc8)[6](this+0xc8, child, this+0x1108, 1); ... }                     // FormContainer::loadForm(child, frmId, show)
  }
  VT(child)[16](child);                    // +0x80 init
  ...
}
```

`StackedSubFormControl` overrides `+0x138` with `FUN_140739580` (creates a `StackedForm`,
0x1298, ctor `FUN_1407533f0`); a `StackedForm` keeps up to N child forms at `+0x258` (0x20 per
slot, count `+0x1258`, current `+0x1288`) and `FUN_14075f320(this, frmId)` selects the visible
one by FRM id. FM8 does not use it for page switching (see 2.5).

### 1.5 FRM id -> C++ class -> FM8FormManager member

`FM8FormManager` ctor `FUN_14010c380` constructs every form in place; `FUN_14011a140`
(`FM8FormManager::loadForms`, called from `FM8Window::open` `FUN_1400c58d0` and on re-init
`FUN_14011b330`) loads them with `FUN_140754cf0(this, &member, frmId, show)` and then calls the
FM8-specific virtual `+0x1b8` (`onFormLoaded`, e.g. FormMain's menu builder `0x14012e310`).

| FRM | Member offset | Class (vtable) | Note |
|---|---|---|---|
| 16 | `+0x12e8` | NGL::Form | helper (hidden, show=0) |
| 22 | `+0x1530` | NGL::Form | |
| 23 | `+0x1778` | NGL::Form | |
| 25 | `+0x19c0` | NGL::Form | |
| 20001 | `+0x1fe0` | FormMainCompact `0x140ab7eb8` | compact header |
| 5 | `+0x1c08` | FormMain `0x140ab7c80` | 948x89 header bar; File button = control id 0x18 |
| 61 | `+0x2330` | FormPageSelectNew `0x140ab8098` | navigator |
| 57 | `+0x2720` | FormFX | |
| 1 | `+0x3650` | FormEasy `0x140ab92b0` | |
| 4 | `+0x4da8` | FormKeyboard `0x140abab58` | |
| 6 | `+0x5538` | FormArp `0x140abae30` | |
| 27 | `+0x5eb8` | FormEffects `0x140abb018` | |
| 32 | `+0x6508` | FormDeepFreq `0x140abb200` | |
| 28 | `+0x9160` | FormOperatorA2F `0x140ab85c0` | shared by ops A..F, op index at `+0x93d8` |
| 31 | `+0xa920` | FormSat `0x140ab87c0` | |
| 30 | `+0xbbb0` | FormFilter `0x140ab8c60` | |
| 29 | `+0xd008` | FormPitch `0x140ab9060` | |
| 33 | `+0xe088` | FormDeepEnv `0x140abb5e8` | |
| 34 | `+0xe490` | FormDeepEnv2 `0x140abb828` | |
| 37 | `+0xf058` | FormDeepSpec `0x140abb3f0` | |
| 36 | `+0xf340` | FormModMatrix `0x140aba490` | |
| 58 | `+0xffb0` | SoundBrowserForm | |
| 59 | `+0x10228` | SoundAttributesForm | |
| 7 | (temp) | NGL::Form | FM matrix template, cloned by `FUN_14012acb0` |

FRM 3 (948x562 composite) has no runtime loader in the exe (no call passes id 3 to
`Form::load`/`loadForm`); the editor is assembled from FRM 5 + 61 + one page + FRM 4 by
`FM8FormManager` (UNCONFIRMED whether FRM 3 is used by the VST3 or design tooling only).
Effect strips (FRM 38-50) load through `SubForm` controls inside FRM 27 (not traced
individually).

## 2. Form and control lifecycle

### 2.1 Structures

`NI::NGL::Form` (size 0x248, ctor `FUN_140752b80`, vt `0x140d4b880`):

| Offset | Field |
|---|---|
| +0x00 | vftable |
| +0x08 | byte loaded |
| +0x0c | int FRM id (-1) |
| +0x10 | std::string name (SSO buf +0x10, size +0x20, cap +0x28) |
| +0x30/+0x34 | int width/height (default 300x200) |
| +0x38/+0x3c | min width/height (when resizable) |
| +0x40 | 8-byte value read/written by slots 10/11 |
| +0x48 | embedded `NI::NGL::DETAIL::PanelItem` (background picture; +0x58 picture id, +0x60 `Picture*`) |
| +0x70 | byte shown; +0x71 resizable; +0x72 transparent; +0x73 picture-load flag |
| +0x78 | `ResourceManager*` (slot 14 setter) |
| +0x80 | `FormContainer*` owner (slot 15 setter) |
| +0x88 | std::list<Control*> sentinel (node = {next, prev, Control*}); +0x90 count. Order = z-order, tail is topmost |
| +0x98 | byte mouse-inside |
| +0xa0 | `Control*` capture; +0xa8 hovered; +0xb0 hover candidate; +0xb8 tooltip control |
| +0xc0 | int render mode (2 = layered cache) |
| +0xc8 | 7 x 0x28 layer records |
| +0x1e0/+0x1e1 | bytes (0x1e1 = 1 suppresses invalidation) |
| +0x1e8 | dirty region |
| +0x210 | list of timers {Control*, interval, countdown} (`FUN_140754470`) ; +0x218 count |
| +0x220 | mouse dispatch stack; +0x228 count |
| +0x230 | map of extra listeners; +0x240 byte multi-listener mode |

`NI::NGL::Control` (size 0xc8, ctor `FUN_140725330`, vt `0x140d45fd0`):

| Offset | Field |
|---|---|
| +0x00 | vftable |
| +0x08 | std::string class name (from registry) |
| +0x28 | std::string (secondary) |
| +0x48/+0x4c/+0x50/+0x54 | int left, top, right, bottom (form-local px) |
| +0x58 | int id |
| +0x60 | std::string tooltip/text (event 0x186a3 returns it) |
| +0x80 | int layer 0..6 (default 3) |
| +0x84 | byte (stream flag) ; +0x88 int (stream) |
| +0x8c | byte focusable; +0x8d byte hovered/focused |
| +0x90 | uint dirty flags (slot 20 ORs, slot 22 clears) |
| +0x94 | byte enabled (hit test); +0x95 byte visible (draw) |
| +0x98 | int value index (-1); +0x9c int |
| +0xa8 | `Form*` parent |
| +0xb0/+0xb8/+0xc0 | std::vector<IListener*> begin/end/cap |

`SwitchControl` extra (creator `FUN_14072c690`, total 0x1d8): +0xc8 `PanelItem` (picture
holder, vt `NI::NGL::DETAIL::PanelItem`); +0xd0 `ResourceRef<Picture,PictureManager>` {vt,
+0xd8 int pictureId, +0xe0 `Picture*`}; +0xe8 int fill mode (1 = solid colour, 2/3 = picture);
+0xec ARGB colour (default `0xffb0b0b0`); +0xf0 `ResourceRef<Font>`; +0x168..+0x174 picture
inset; +0x178 byte hover-frames; +0x179 pressed-frames; +0x17a disabled-frame; +0x17b toggles;
+0x17c left-button-only; +0x17d double-click-as-press; +0x17e; +0x17f mute-notify; +0x180
std::string text/URL; +0x1a0 int mode; +0x1a4 int repeat delay; +0x1a8 int; +0x1ac/+0x1b0 press
offset; +0x1b4 int value; +0x1b8 int frames per state group; +0x1bc pressed count; +0x1c0 hover
count; +0x1c4 repeat countdown; +0x1c8 byte pressed; +0x1cc; +0x1d0 int -1.

Creator defaults: mode 0, `+0x17b` toggles = 1, `+0x17c` left-only = 1, `+0x17d` = 1,
fill mode 1 with colour `0xffb0b0b0`, value 0.

Press (`FUN_14073f550`, event 1000 action 1): capture for modes 0/1/2/4; toggle `+0x1b4` if
`+0x17b && (!+0x179 || mode != 4)`; notify `0x186a1` unless mode 4 or `+0x17f`. Release
(`FUN_14072fba0`, action 2): if `+0x179`, toggle again when `+0x17b && inside && mode != 0`;
mode 2 releases capture; otherwise notify `0x186a1` when the pointer is still inside and mode
!= 0; mode 5 opens `+0x180` as a URL. Net behaviour per mode: 0 toggle+notify on press only
(FormMain's File button); 1 toggle+notify on press, notify again on release (with `+0x179`
set the value toggles back on release, i.e. momentary); 2 like 1 plus auto-repeat from a form
timer (`+0x1a4` delay); 3 notify on press without capture; 4 one toggle and one notify on
release inside (standard push button); 5 hyperlink.

`FormContainer` (0x1040, ctor `FUN_140752d40`, vt `0x140d4ba40`): +0x10 128 slots x 0x20
`{Form* +0x00, byte +0x08, rect +0x0c..+0x1c}`, +0x1010 count, +0x1014 owns-forms, +0x1028
hovered form. `FormManager` adds +0x1048 `Context*` (`+0x10` `NGL::Window*`, `+0x18`
`ResourceManager*`; returned by slot 37 +0x128), +0x1068 pointer (slot 24), +0x1070 embedded
root `Form`, +0x12b8 focus control id, +0x12bc/+0x12c0 root origin, +0x12c4/+0x12c5 flags.
`FM8FormManager` adds +0x12e0 `FM8VstObject*` (the app object: `+0x55d0` `FM8EditBuffer*`,
`+0x55e0` `FM8Midi*`, `+0x5620` `FM8FormManager*`), the forms of 1.5, +0x104a0 vector of
operator/effect pages, +0x104f8 current page `Form*`, +0x10500 current page id, +0x10504
previous, +0x10514 forms-loaded, +0x10515 animated-swap, +0x10516 keyboard shown, +0x10540
hovered `ParameterLink`'s control.

### 2.2 Vtable maps

Form (`0x140d4b880`); FM8 forms extend it with +0x1b8 `onFormLoaded` (+0x1c0/+0x1c8 extra).

| Slot | Function | Role |
|---|---|---|
| 0 | `FUN_140753ae0` | deleting dtor (`FUN_140753540` body: detaches and deletes every control, `VT(c)[24]; VT(c)[0](c,1)`) |
| 1 +0x08 | `FUN_140749da0` | `handleEvent` thunk -> slot 3 |
| 3 +0x18 | `FUN_14075a220` | `onEvent(type, EventData*)` |
| 5 +0x28 | `FUN_140754bc0` | `load(frmId)` |
| 6 +0x30 | `FUN_14075ef40` | `setSize(int64* wh)`; 9 (+0x48) forwards here |
| 7 +0x38 | `FUN_140760090` | `show(bool)`; false clears capture, sends 0x3ed to hovered |
| 8 +0x40 | `FUN_140757ff0` | `getSize` |
| 12 +0x60 | `FUN_1407583f0` | `isTransparent` |
| 13 +0x68 | `FUN_140757430` | opaque-region query |
| 14 +0x70 | `FUN_14075fb40` | `setResourceManager` |
| 15 +0x78 | `FUN_14075f6b0` | `setContainer` |
| 16 +0x80 | `FUN_1407580b0` | `init`: resolves background picture, adjusts size, `container->VT[25](form)`, sets loaded=1, attaches every control (`VT(c)[23](c, form)`) |
| 17 +0x88 | `FUN_140755040` | `unload` (loaded=0, drops focus/capture) |
| 19 +0x98 | `FUN_1407583b0` | `invalidateRect(rect*)` |
| 20 +0xa0 | `FUN_140758390` | `invalidate` |
| 21 +0xa8 | `FUN_140756e30` | `collectDirtyRegion(region*)` |
| 23 +0xb8 | `FUN_140753dc0` | `addControl(Control*)`: setEnabled(1), setVisible(1), append, attach if loaded, invalidate |
| 24 +0xc0 | `FUN_140756160` | bring control to front (within its layer) |
| 25 +0xc8 | `FUN_140755220` | `removeControlById(id)` (detaches and unlinks; does not delete) |
| 27 +0xd8 | `FUN_14075e3b0` | remove all controls |
| 28 +0xe0 | `FUN_140758c60` | move control to end (layer change) |
| 29/30 +0xe8/+0xf0 | `FUN_140758720`/`FUN_140758e30` | control enabled/visible changed |
| 31 +0xf8 | `FUN_140758d30` | `setFocus(ctrl, bool)` -> container +0x78 |
| 37 +0x128 | `FUN_140759060` | `draw(GC, Region*)` |
| 38 +0x130 | `FUN_14075d6a0` | `readFromResource(stream)` |
| 51 +0x198 | `FUN_14075c1a0` | mouse dispatch (events 1000..1005) |
| 52 +0x1a0 | `FUN_14075c080` | `onResized(oldSize)` |
| 54 +0x1b0 | `FUN_140759bd0` | `drawBackground` |

Control (`0x140d45fd0`); overrides listed for Switch (S), SubForm (F), ValueEdit (V),
ButtonMenu (B), PopupMenu (P).

| Slot | Base | Role / overrides |
|---|---|---|
| 0 | dtor | S `FUN_1407295c0`, B `FUN_1407539c0` |
| 1 +0x08 | `FUN_140749da0` | `handleEvent` thunk -> slot 3 |
| 3 +0x18 | `FUN_14073e670` | `onEvent(type, EventData*)`; S `FUN_14073f550`, F `FUN_14073f390`, V `FUN_140740130`, B `FUN_140759de0`, P `FUN_14075abd0` |
| 5 +0x28 | `FUN_14074b1d0` | `setId(int)` -> +0x58 |
| 6 +0x30 | `FUN_14074ae40` | `setLayer(uint)` |
| 7 +0x38 | `FUN_140733b90` | `bringToFront` |
| 8 +0x40 | `FUN_14074bc70` | `setRect(int[4])` (invalidates old and new) |
| 9 +0x48 | `FUN_140738a00` | `hasTransparency` (S `FUN_140738a10`) |
| 10 +0x50 | `FUN_140735f70` | opaque region (F `FUN_140736290`) |
| 11 +0x58 | `FUN_14074ab40` | `setEnabled(bool)` +0x94 |
| 12 +0x60 | `FUN_14074e5b0` | `setVisible(bool)` +0x95 |
| 14 +0x70 | `FUN_140733de0` | `getValue(idx)` (+0x98) |
| 17 +0x88 | `FUN_140749df0` | set +0x98 |
| 20 +0xa0 | `FUN_140738620` | `invalidate(mask)` (0 = default) |
| 21 +0xa8 | `FUN_140734e30` | `collectDirty(region*, flags)` |
| 22 +0xb0 | `FUN_1407492a0` | clear dirty |
| 23 +0xb8 | `FUN_1407402e0` | `attach(Form*)` (+0xa8); S `FUN_140740e40` (resolves picture, recomputes frame groups), F `FUN_140740c50`, V `FUN_1407410b0`, B `FUN_14075bb60` |
| 24 +0xc0 | `FUN_140739690` | `detach` (+0xa8=0); F `FUN_140739c40`, S `FUN_140739cc0` |
| 26 +0xd0 | pure | `paint(GC)`: S/B `FUN_14073b620`, V `FUN_14073b9b0`, P `FUN_1407597a0` |
| 27 +0xd8 | `FUN_14073a2b0` | `draw(GC, Region*)`: per clip rect push clip (`FUN_14030deb0`), call slot 26, restore; then debug frame if `DAT_141201720` |
| 29 +0xe8 | `FUN_14072a1e0` | `addListener(IListener*)` -> vector +0xb0 |
| 30 +0xf0 | `FUN_140749110` | `removeListener` |
| 31 +0xf8 | `FUN_140746030` | `readFromStream`; S `FUN_140747510`, F `FUN_1407473d0`, V `FUN_140748740`, B `FUN_14075d2d0`, P `FUN_14075d8f0` |
| 32 +0x100 | `FUN_1407515e0` | `writeToStream` (version 7) |
| 35 +0x118 | `FUN_140738da0` | `notify(type, data*, stopOnFirst)` |
| 36 +0x120 | `FUN_140748df0` | tooltip notify (0x186a3) |
| 39 +0x138 | | F `createSubForm` `FUN_140739630`; V mouse handler `FUN_1407438a0` |
| 40 +0x140 | | V key handler `FUN_1407422a0` |

FormContainer (`0x140d4ba40`) / FormManager (`0x140d4bc30`) / FM8FormManager (`0x140abba20`):

| Slot | Function | Role |
|---|---|---|
| 3 +0x18 | `FUN_140754060` | `addFormWithRect(form, rect*, show)` (sets manager from context+0x18 if none, `form->setContainer`, `setSize`, `show`, then slots 25 and 9) |
| 4 +0x20 | `FUN_140754150` | `addForm(form, show)` (stacked below existing) |
| 5 +0x28 | `FUN_140754c60` | `loadFormWithRect(form, id, rect*, show)` |
| 6 +0x30 | `FUN_140754cf0` | `loadForm(form, id, show)` = `form->load(id)` + slot 4 |
| 7 +0x38 | `FUN_14075e7c0` | `removeForm(form)` (deletes if +0x1014) |
| 9 +0x48 | `FUN_140755400` (mgr) | relayout root form to bounding box |
| 10 +0x50 | `FUN_140760200` | `showForm(form, bool)` |
| 11 +0x58 | `FUN_140760740` | swap/replace form (animated page change) |
| 25 +0xc8 | `FUN_14075b3e0` | `onFormAdded` (fits slot rect to form size) |
| 26 +0xd0 | `FUN_14075bf70`; FM8 `FUN_14012b830` | position a form (FM8 switches on `form+0xc` FRM id: pages 1,6,27-34,36,37,57-59 go to (`+0x2360`,`+0x1c3c`); 4 and 18 below; 61 at x=0) |
| 37 +0x128 | `FUN_14011fa40` | `getContext` (+0x1048) |
| 40 +0x140 | `FUN_14075a940`; FM8 `FUN_14012a250` | `onEvent` (root form first, then mouse dispatch) |
| 41 +0x148 | `FUN_14075c940` | mouse dispatch over form slots (enter/leave 0x3ec/0x3ed, `FUN_140752aa0` translates and calls `form->VT[1]`) |

### 2.3 Construction, init, lookup

* Construction: forms are members (FM8) or `new`ed (`SubFormControl`); controls come from the
  registry creators (heap, `FUN_140a0f284` = operator new).
* `Form::getControl(id)` = `FUN_140756910(form, int id)` (non-virtual, linear scan of the
  +0x88 list comparing `Control+0x58`). `FormMain::init` (`FUN_140120340`, Form slot 16
  override) uses it for control 0x17 then calls `FUN_1407580b0` and `FUN_1401207b0(this+0x250,
  this)` (bind all parameter links). The menu builder (`FUN_14012e310`, FormMain +0x1b8)
  caches controls 0x11, 0x10, 0xf, 0x16, 3, 0x1b, 0x18 (File button -> `FormMain+0x2c8`),
  0x13, 6 and fills `FormMain+0x2c8 -> +0x1e0` with `PopupMenu::addItem` (`0x140753f10`).
* Attach order: `Form::init` attaches existing controls; `addControl` attaches immediately if
  the form is already loaded (`Form+0x08`). Attach is where pictures are resolved
  (`FUN_140737fb0(ctrl, ctrl+0xc8)` -> `PictureManager::get(id)` = `VT(pm)[4](pm, id, flag)`).

### 2.4 Layout

Control rects are the FRM rects (form-local). `SubFormControl` places its child form at its own
rect (child size = rect size; events translated by `+0x48/+0x4c`). Forms are placed by
`FormManager::moveForm` `FUN_14075f740(mgr, form, int64* xy)` into the slot rect; FM8
repositions pages in `FUN_14012b830`. Resizing a form calls slot 52 with the old size.

### 2.5 Page switching

`FUN_14012fda0(FM8FormManager*, int pageId)` maps navigator ids to members (8 FX, 0xb Easy,
0x15 Arp, 0x17 Effects, 0x19 DeepFreq, 0x1b DeepEnv, 0x1c DeepEnv2, 0x1e ModMatrix, 0x1f
DeepSpec, 0x20..0x25 OperatorA2F with op index `id-0x20`, 0x26 Sat, 0x27 Filter, 0x28 Pitch,
0x2a SoundBrowser, 0x2b SoundAttributes), then hides the current page (`+0x104f8`) and shows
the new one through `FormContainer::showForm` (`FUN_140760200`) or the animated swap
(`FUN_140760740`, when `+0x10515`), calls the form's slot 18, resets the hovered link
(`FUN_140130010(mgr, 0, ...)`) and records `+0x10500/+0x10504`. Keyboard visibility toggles
FRM 4 vs FRM 25 with `showForm` (`FUN_14011a140`). StackedForm is not involved.

### 2.6 Destruction

`Form` dtor (`FUN_140753540`): for every list node, `VT(ctrl)[24](ctrl)` (detach) then
`VT(ctrl)[0](ctrl, 1)` (delete); the form owns its controls. `SubFormControl` dtor
(`FUN_140727fc0`) unloads (`VT(child)[17]`) and deletes the child form. `FormContainer` dtor
(`FUN_14075eba0`) unloads and deletes owned forms. To remove a control at runtime use Form slot
25 (`removeControlById`) then delete it yourself.

## 3. Event flow

### 3.1 Window to control

1. Win32 message in the `NI::UIA` window procedure (`FUN_1407863a0`; `WM_LBUTTONDOWN` 0x201 etc.
   `SetCapture`), window classes `NIChildWindow%p` / `NIVSTChildWindow%p` registered by
   `FUN_140782120` (VST editor = `NI::NGL::Window<NI::UIA::ChildWindow>`, vt `0x140d44790`;
   standalone = `FM8Window` : `Window<StandaloneWindow>`, ctor `FUN_1400c56d0`, which also
   allocates the `FM8FormManager`).
2. `NGL::Window::onEvent` `FUN_1400c1e80` maps UIA events to NGL ids (UIA 0x3e9 -> NGL 0x3ea
   move; 1000000 resize; 0xf4241 idle; 0xf4242 paint; 0xf4243 flush) and calls the
   `FormManager` (`Window-0x18` relative to the sub-object) slot 40.
3. `FormManager::onEvent` `FUN_14075a940` (FM8 wrapper `FUN_14012a250` also handles 2 = timer,
   0xf4240/0xf4241 hooks): root form first, then `FUN_14075a620` -> slot 41
   `FUN_14075c940` hit-tests form slots (topmost first), tracks the hovered form (+0x1028,
   sending 0x3ec enter / 0x3ed leave) and calls `FUN_140752aa0(mgr, form, ev, data)`, which
   translates coordinates and calls `form->VT[1]`.
4. `Form::onEvent` `FUN_14075a220` routes 1000..1099 to slot 51 `FUN_14075c1a0`: if a capture
   control (`+0xa0`) exists it gets everything; otherwise iterate controls from the tail
   (topmost) with `+0x94` set and rect containing the point, call `ctrl->VT[1](ctrl, ev,
   data)` until one returns true; hover changes send 0x3ed/0x3ec to the old/new control.
5. The control handles it (Switch: `FUN_14073f550`).

Event ids seen: `2` timer tick (form timers, `FUN_140754470(form, ctrl, delay, ...)`); `1000`
mouse button, sub-action in `EventData+0x10` (1 down, 2 up, 3 double-click), button in
`+0x0c` (1 = left); `1002` (0x3ea) mouse move; `1003` (0x3eb) delivered to the focus control
(UNCONFIRMED: wheel); `1004`/`1005` enter/leave; `1100..1199` keyboard (ButtonMenu opens on key
1); `1200..1299` other input; `1300` (0x514) query at point; `100001` (0x186a1) activate via
keyboard; `100002` hover-state; `100003` tooltip text query; `100004` hover changed
(`data[0]==this`); `1000000` resize; `1000001` idle; `1000002` paint; `1000003` flush.
`EventData` mouse layout: `+0x00 int x, +0x04 int y, +0x0c int button, +0x10 int action`
(others UNCONFIRMED).

### 3.2 Control -> listener notification

`Control::notify(type, data, stopOnFirst)` = slot 35 `FUN_140738da0`:

```c
for (l in vector[+0xb0..+0xb8]) r |= VT(l)[0](l, *(int*)(this+0x58) /*ctrlId*/, type, data);
```

Listener interface: vtable slot 0 = `bool onControlNotify(this, int ctrlId, int type, void*
data)`. `FormMain` implements it through its secondary vtable at `FormMain+0x248`
(`0x140ab7e48`, slot 0 = command sink `FUN_140123960`); the audio-settings dialog does the same
at `+0x248` (`FUN_1400d1a60`). Notification types and their `data`:

| Type | Meaning | data |
|---|---|---|
| `0x186a1` (100001) | value changed / clicked | `{int srcEvent (1000 mouse, 2 timer, 0x186a1 key); void* ev; int value}` for Switch/ButtonMenu (value = `+0x1b4`); ValueEdit puts a 16-byte `Value {int kind; ...; float v @+8}` at `data+0x10`; PopupMenu puts the item index at `data+0x10` |
| `0x186a2` (100002) | hover/focus state changed | `data+0x10` = bool; ParameterLink uses it to select the parameter for the display and CC feedback |
| `0x186a3` (100003) | tooltip query | |
| `0x186a4` (100004) | gesture begin (capture taken, `FUN_14074ef70`) | ParameterLink -> `FUN_140845540` beginEdit |
| `0x186a5` (100005) | gesture end (`FUN_14072b5a0`) | ParameterLink -> `FUN_140844c20` endEdit |
| `0x18705` (100101) | ButtonMenu about to open | `{1000, ev, PopupMenu*}`, `stopOnFirst=1`; the sink rebuilds the Options menu for control 0x19 here (`FUN_14011e950`) |

Switch click: `FUN_14073f550` on 1000/down sets `+0x1c8`, toggles `+0x1b4` if `+0x17b`,
`notify(0x186a1, {1000, ev, value}, 0)` unless mode 4; on up `FUN_14072fba0` notifies again
(modes != 0) then `FUN_14072b5a0` -> 0x186a5.

ButtonMenu click: `FUN_140759de0` notifies `0x18705`, then `FUN_14075d060(&menu(+0x1e0),
xy, parentForm, 1, 0)` opens the popup below/beside the button (`+0x3b8` placement). The
embedded `PopupMenu` (a Control; ctor `FUN_140753060`; docs/hooks.md has addItem etc.) has the
button's id and the button's listener interface (`+0x1d8`) registered at attach
(`FUN_14075bb60`). On selection it notifies `0x186a1` with the index at `data+0x10`; the
button's listener `FUN_1407587a0` stores the index (`FUN_14074b370`) and re-notifies its own
listeners with `0x186a1` and `data = {0x186a1, innerData, value}`, so the sink reads the
menu via `*(data+8)`: `+0x10` index, `+0x18` `PopupMenu*`, command id at
`*(menu+0x158)[idx] + 0x60` (matches docs/hooks.md).

### 3.3 ParameterLink: control id -> synth parameter

`IParameterLink` (vt `0x140ab6e10`, 0x30+ bytes): `+0x08` FM8 app object, `+0x10`
`FM8EditBuffer*`, `+0x18` `Control*`, `+0x20` int control id, `+0x24` int parameter tag (or
index), `+0x28` byte focusable, `+0x29` byte in-gesture. Links are embedded in each form and
pushed into the form's `ParamLinksOwner` vector (FormMain `+0x260`) in the ctor, e.g.
`FormMain` ctor `FUN_140112d30` sets `MorphSelectorLink` (ctrl 0x17, +0x334 = 0xc) and
`ArpSwitchParameterLink` (ctrl 0x5c); `FM8FormManager` ctor sets `PtMorphedParameterLink`
(ctrl 0x2d, tag 0x9c) for FormEffects.

| Slot | Function | Role |
|---|---|---|
| 0 | `FUN_140125850` (base); Switch links `FUN_140121340`; ValueEdit `FUN_1401213e0` | `onControlNotify`: 0x186a2 -> slot 3; 0x186a4/0x186a5 begin/endEdit; Switch: 0x186a1 -> `slot 8(data+0x10)`; ValueEdit: 0x186a1 -> `slot 8(Value @ data+0x10)` |
| 1 +0x08 | `FUN_14011fe30` (ValueEdit `FUN_14011ff60`) | `bind(Form*, fm8obj)`: `ctrl = getControl(+0x20)`, `ctrl+0x8c = focusable`, `ctrl->addListener(this)`; ValueEdit also sets range/format |
| 2 +0x10 | `FUN_140131d20` | refresh control from engine (`FUN_14074b370(ctrl, slot7())`) |
| 3 +0x18 | `FUN_140130150` | hover-select: `FM8FormManager+0x10540 = ctrl`, `FUN_140130010`, `FUN_1400f56e0(FM8Midi, desc)` which emits the CC feedback of the hovered parameter through `0x1400f5370` when Dump-Ctrls is on |
| 6 +0x30 | | `getTag` |
| 7 +0x38 | `FUN_14011f200` | engine value: `VT(eb)[11](eb, tag)` (+0x58 getParameterByTag) |
| 8 +0x40 | `FUN_140130790` / ValueEdit `FUN_140130d00` | set engine: `FUN_1400f7c70(eb, tag, (float)v, 1)` |

`ParamLinksOwner::bindAll` `FUN_1401207b0(owner, form)` calls slot 1 on every link; FormMain's
third vtable (`+0x250`, `0x140ab7e58`) slot 0 `FUN_1401335d0` is the idle refresh (calls slot 2
on all links, updates CPU `%3d%%`, `Init Sound`).

The UI-side setter `FUN_1400f7c70(FM8EditBuffer* eb, int tag, float v, char thread=1)`:
`VT(eb)[2](eb, tag, v, 1)` = `setParameterByTag` (`0x1400f5970`, docs/hooks.md) and, if it
returns true, `FUN_1400e2d70(eb, *(eb+0x68+tag*4), &{0, tag, 0, -1})` to broadcast to the
host/automation. `FM8EditBuffer* = *(fm8obj+0x55d0)` (`FUN_1401029b0`), `fm8obj =
*(FM8FormManager+0x12e0)`.

## 4. Rendering

* Backend: software. `NI::UIA::Graphics` draws into a 32-bit top-down DIB; the window's
  `WM_PAINT` handler `FUN_1407856a0` does `BeginPaint`, binds the HDC (`FUN_140778640`),
  dispatches NGL event `0xf4242` (paint), `EndPaint`. Flush is `FUN_14077e1b0` with
  `SetDIBitsToDevice` (1:1) or `StretchDIBits` (HiDPI scale at `+0x44`, `SetStretchBltMode
  HALFTONE`); `BITMAPINFOHEADER{biBitCount=32, biHeight=-h}`. Resize (`0xf4240`) reallocates
  the surface (`FUN_140795e60`) and repaints everything (`FUN_140759360(mgr, gfx, region,
  3)`); `0xf4243` repaints only the dirty region collected by `FUN_140757bc0`. `wglMakeCurrent`
  exists only in `FUN_140909880`, not on this path; no Direct2D/D3D.
* Form draw = Form slot 37 `FUN_140759060(form, GC, Region*, ...)`: slot 54 background, then
  every control in list order (bottom to top) with `+0x95` set whose rect intersects the
  region: clip = region ∩ rect, `ctrl->VT[27](ctrl, GC, &clip)`. Render mode 2 uses the
  per-layer cache at `+0xc8` keyed by `Control+0x80`.
* Control draw = slot 27 `FUN_14073a2b0` (clip loop) -> slot 26 `paint(GC)`. Switch paint
  `FUN_14073b620`: picks a frame index from value `+0x1b4`, hover (`+0x1c0` && `+0x178`, +1
  group), pressed (`+0x1bc` && `+0x179`, +2 groups), disabled (last frame if `+0x17a`), clamps
  to `+0x1b8` frames per group (computed by `FUN_140737ed0` from `Picture+0x10` frame count),
  shifts the rect by `+0x1ac/+0x1b0` while pressed, and blits through the `PanelItem`
  (`FUN_1407305a0(this+0xc8, GC, &rect, frame, &rect, 0, flag)` -> `FUN_140730160`). Fill mode
  `+0xe8 == 1` paints the solid colour `+0xec` instead of a picture.
* Dirty tracking: `Control::invalidate` (slot 20) ORs `+0x90`; `Form::collectDirtyRegion`
  (slot 21) unions the rects of controls whose flags are set (`FUN_140733d30` reads and clears
  them); the window repaints that region.
* Mouse handling: Control slot 3 (+0x18) via the slot 1 thunk; Form slot 51 (+0x198)
  dispatches; FormContainer slot 41 (+0x148) dispatches across forms.

## 5. Recipe for FM8.plus

Both recipes run inside a detour on FormMain's `onFormLoaded` (Form slot 55 +0x1b8 =
`0x14012e310` exe / `0x18011dd00` vst2, already hooked per docs/hooks.md), after calling the
original: the form is loaded, attached and has a ResourceManager, so `addControl` attaches and
resolves pictures immediately. `this` is `FormMain` (`Form*`). Function pointers are exe VAs;
vst2 in section 7.

### 5.1 Add a native Switch to FormMain

```c
typedef void* (*CreateByName)(const char*);                     // 0x14072c990
typedef void  (*AddControl)(void* form, void* ctrl);            // 0x140753dc0 (Form slot 23)
struct IListenerVT { bool (*onNotify)(void* self, int ctrlId, int type, void* data); };
struct MyListener { const IListenerVT* vt; /* state */ };

void* sw = CreateByName("Switch");                              // 0x1d8 bytes, NI::NGL::SwitchControl
int rect[4] = { x0, y0, x1, y1 };                               // FormMain-local px (FRM 5 is 948x89)
((void(*)(void*,int*))VT(sw)[8])(sw, rect);                     // +0x40 setRect
((void(*)(void*,int))VT(sw)[5])(sw, 0x1000);                    // +0x28 setId (pick an id unused in FRM 5)
*(int*)((char*)sw + 0x1a0) = 4;                                 // mode 4: one toggle + one notify on release inside (push button)
*(char*)((char*)sw + 0x17b) = 1;                                // latch value 0/1 (creator default, shown for clarity)
*(int*)((char*)sw + 0x1b4) = initialValue;
// look: either an existing PICTURE strip (frames stacked vertically: [states][hover][pressed][disabled])
*(int*)((char*)sw + 0xd8) = PICTURE_ID;  *(int*)((char*)sw + 0xe8) = 2;  *(char*)((char*)sw+0x178) = 1; *(char*)((char*)sw+0x179) = 1;
// or, with no picture at all, a flat ARGB rectangle (creator default): +0xe8 = 1, +0xec = 0xffb0b0b0
((void(*)(void*,void*))VT(sw)[29])(sw, &myListener);            // +0xe8 addListener
((AddControl)0x140753dc0)(formMain, sw);                        // enables, shows, appends (topmost), attaches (loads picture via *(form+0x78)+0x10), invalidates
```

Listener: `onNotify(self, 0x1000, 0x186a1, data)` fires once per click (on release, mode 4)
with `*(int*)(data+0x10)` = new value (0/1); return true to stop other listeners. Use mode 0 if
the action should fire on press like FormMain's own buttons. Programmatic value change:
`FUN_14074b370(sw, v)` (`0x14074b370`; sets `+0x1b4` and invalidates). The form owns the control
and deletes it in its dtor; to remove early call Form slot 25 `(form, 0x1000)` then
`VT(sw)[0](sw, 1)`. Hit-testing works because the control is in the `+0x88` list with `+0x94`
set and its rect; drawing works because `+0x95` is set and `addControl` invalidated the rect.
For a text label add a `"Label"` control the same way (text setter `FUN_14074cd60(ctrl,
std::string*)` UNCONFIRMED for Label; it is the ValueEdit setter). Whether Switch draws its
`+0x180` string is UNCONFIRMED (its paint does not reference it).

Precedent in FM8 itself: `FUN_14012acb0` creates controls at runtime (clones from FRM 7, sets
rect/id, `VT(form)[23]`, attaches, adds a listener), and `FUN_1400d1a60` looks controls up by id
and registers `this+0x248` as listener.

### 5.2 Open a native PopupMenu from the click

Option A (cleanest, all native): create a `"ButtonMenu"` instead of a Switch. It is a
SwitchControl (same fields as 5.1) with a `PopupMenu` at `+0x1e0` that opens itself on click and
reports the selection through the button's listeners:

```c
void* bm = CreateByName("ButtonMenu");                           // 0x3c8 bytes, NI::NGL::ButtonMenuControl (same class as FormMain's File button)
/* rect, id, picture as in 5.1; ButtonMenu keeps +0x1a0 mode 0 (opens on down) */
char* menu = (char*)bm + 0x1e0;                                  // embedded NI::NGL::PopupMenu (0x1d8)
PopupMenu_addItem(menu, "FM8.plus", "FM8.plus", 0x1000);         // 0x140753f10
PopupMenu_addSeparator(menu);                                    // 0x140754410
PopupMenu_setItemCheckState(menu, 0, 1);                         // 0x14075fd30
((void(*)(void*,void*))VT(bm)[29])(bm, &myListener);
((AddControl)0x140753dc0)(formMain, bm);                         // attach wires menu id + button listener (FUN_14075bb60)
// listener: type 0x186a1, ctrlId = button id; inner = *(void**)(data+8); idx = *(int*)(inner+0x10); menuPtr = *(void**)(inner+0x18);
// cmd = *(int*)(*(char**)(menuPtr+0x158)[idx] + 0x60)   (item vector at menu+0x158, MenuItem+0x60 command id)
// type 0x18705 arrives first (data+0x10 = PopupMenu*) if the menu must be rebuilt before opening.
```

Option B (Switch + manual popup): on `0x186a1` from the Switch, build a menu and show it:

```c
void* menu = op_new(0x1d8); PopupMenu_ctor(menu);                // 0x140753060 (vst2 0x18070a6d0)
((void(*)(void*,int))VT(menu)[5])(menu, 0x1001);                 // PopupMenu is a Control: give it an id
((void(*)(void*,void*))VT(menu)[29])(menu, &menuListener);       // selection arrives as 0x186a1, idx at data+0x10
PopupMenu_addItem(menu, ...);
int64 xy = (int64)*(int*)((char*)sw+0x48) | ((int64)*(int*)((char*)sw+0x54) << 32);   // below the switch, form-local
((void(*)(void*,int64,void*,char,char))0x14075d060)(menu, xy, formMain, 1, 0);      // PopupMenu::popup(menu, xy, form, asChild, byKeyboard)
```

`popup` needs the form's container context (`*(form+0x80)->VT[37]()->+0x10` = the NGL
window) to create the popup window and converts `xy` to screen space; it records the active
menu in `DAT_141201b30`. Delete the menu after it closes (UNCONFIRMED: who frees a heap
PopupMenu after selection; the embedded menu of Option A avoids the question).

### 5.3 Writing a parameter from a new control

`eb = *(void**)(*(void**)(FM8FormManager+0x12e0) + 0x55d0)`; `FM8FormManager` is
`*(form+0x80)` for any FM8 page form (the manager is the container). Then
`((bool(*)(void*,int,float,char))0x1400f7c70)(eb, tag, value, 1)` (goes through
`setParameterByTag` and broadcasts), or mimic a link: `VT(eb)[2](eb, tag, value, 1)`.

## 6. Struct offsets cheat sheet

| Object | Field | Offset |
|---|---|---|
| Form | loaded / FRM id / w,h | +0x08 / +0x0c / +0x30,+0x34 |
| Form | ResourceManager / container | +0x78 / +0x80 |
| Form | control list sentinel / count | +0x88 / +0x90 (node: next, prev, Control* @+0x10) |
| Form | capture / hovered control | +0xa0 / +0xa8 |
| Form | secondary vtable (FM8 forms, listener) | +0x248 |
| Control | class name string / rect / id | +0x08 / +0x48..+0x54 / +0x58 |
| Control | layer / enabled / visible / dirty | +0x80 / +0x94 / +0x95 / +0x90 |
| Control | parent Form / listener vector | +0xa8 / +0xb0..+0xb8 |
| Switch | picture id / Picture* / fill mode / colour | +0xd8 / +0xe0 / +0xe8 / +0xec |
| Switch | mode / value / toggles / flags | +0x1a0 / +0x1b4 / +0x17b / +0x178..+0x17f |
| ButtonMenu | listener iface / PopupMenu / placement | +0x1d8 / +0x1e0 / +0x3b8 |
| PopupMenu | item vector / owner | +0x158 / +0xd8 (docs/hooks.md) |
| SubFormControl | container / child FRM id / child Form* / name | +0xc8 / +0x1108 / +0x1110 / +0x1118 |
| FormContainer | slots / count / hovered form | +0x10 (0x20 each) / +0x1010 / +0x1028 |
| FormManager | context / root form / focus id | +0x1048 / +0x1070 / +0x12b8 |
| FM8FormManager | app object / FormMain / navigator / current page | +0x12e0 / +0x1c08 / +0x2330 / +0x104f8 |
| IParameterLink | app / EditBuffer / Control / ctrlId / tag | +0x08 / +0x10 / +0x18 / +0x20 / +0x24 |
| EventData (mouse) | x / y / button / action | +0x00 / +0x04 / +0x0c / +0x10 |
| Notify data (0x186a1) | src event / ev ptr / value | +0x00 / +0x08 / +0x10 |

## 7. exe -> vst2 address table

Exact = normalized decompiled body identical and same size; "sz" = same size, body matched by
prefix only (verify with the listed anchor before hooking); "str" = confirmed by a string.

| Role | exe | vst2 | Basis |
|---|---|---|---|
| FindResourceA loader | `0x1408f85f0` | `0x18089d710` | exact |
| resource stream factory | `0x1408db5a0` | `0x180880d60` | exact |
| ResourceManager::loadResource | `0x140766230` | `0x18071d6e0` | sz |
| Form::load | `0x140754bc0` | `0x18070c180` | exact |
| Form::readFromResource | `0x14075d6a0` | `0x180714c60` | str |
| control factory (per record) | `0x14075dcf0` | `0x1807152b0` | sz, calls verified |
| registry create by name | `0x14072c990` | `0x1806e4000` | sz, body verified |
| registry registration | `0x1407374e0` | `0x1806eeaf0` | str "SubForm" |
| Control::readFromStream | `0x140746030` | `0x1806fd640` | str |
| Switch::readFromStream | `0x140747510` | `0x1806feb20` | str |
| SubForm::readFromStream | `0x1407473d0` | `0x1806fe9e0` | sz |
| SubForm::attach | `0x140740c50` | `0x1806f8260` | exact |
| FormContainer::loadForm | `0x140754cf0` | `0x18070c2b0` | sz, body verified |
| FormContainer::addFormWithRect / addForm / loadFormWithRect / removeForm | `0x140754060` / `0x140754150` / `0x140754c60` / `0x14075e7c0` | `0x18070b620` / `0x18070b710` / `0x18070c220` / `0x180715d80` | exact |
| FormContainer::showForm | `0x140760200` | `0x180717730` | sz |
| FormManager::moveForm | `0x14075f740` | `0x180716c70` | exact |
| FormManager::onEvent / mouse dispatch | `0x14075a940` / `0x14075c940` | `0x180711f00` / `0x180713f00` | sz / exact |
| Form ctor / init / unload / show / setSize | `0x140752b80` / `0x1407580b0` / `0x140755040` / `0x140760090` / `0x14075ef40` | `0x18070a1f0` / `0x18070f670` / `0x18070c600` / `0x1807175c0` / `0x180716500` | str / exact / exact / exact / sz |
| Form::getControl / addControl / removeControlById | `0x140756910` / `0x140753dc0` / `0x140755220` | `0x18070ded0` / `0x18070b430` / `0x18070c7e0` | exact |
| Form::onEvent / mouseDispatch | `0x14075a220` / `0x14075c1a0` | `0x1807117e0` / `0x180713760` | exact |
| Form::draw / drawBackground / collectDirty | `0x140759060` / `0x140759bd0` / `0x140756e30` | `0x180710620` / `0x180711190` / `0x18070e3f0` | exact |
| Control ctor / onEvent / setRect / setId | `0x140725330` / `0x14073e670` / `0x14074bc70` / `0x14074b1d0` | `0x1806dc9a0` / `0x1806f5c80` / `0x180703220` / `0x180702780` | exact |
| Control::setEnabled / setVisible / setLayer | `0x14074ab40` / `0x14074e5b0` / `0x14074ae40` | `0x180702150` / `0x180705b40` / `0x180702450` | sz |
| Control::addListener / removeListener / notify | `0x14072a1e0` / `0x140749110` / `0x140738da0` | `0x1806e1850` / `0x180700720` / `0x1806f03b0` | exact |
| Control::draw (clip loop) | `0x14073a2b0` | `0x1806f18c0` | sz |
| Switch creator / onEvent / release / paint / attach / setValue | `0x14072c690` / `0x14073f550` / `0x14072fba0` / `0x14073b620` / `0x140740e40` / `0x14074b370` | `0x1806e3d00` / `0x1806f6b60` / `0x1806e7210` / `0x1806f2c30` / `0x1806f8450` / `0x180702920` | exact / exact / str / exact / exact / sz |
| gesture begin / end notify | `0x14074ef70` / `0x14072b5a0` | `0x180706500` / `0x1806e2c10` | exact |
| ResourceRef::load / PanelItem draw / recompute frames | `0x140131100` / `0x1407305a0` / `0x140737ed0` | `0x180120a60` / `0x1806e7c10` / `0x1806ef4e0` | exact |
| ButtonMenu creator / onEvent / attach / listener | `0x140754d70` / `0x140759de0` / `0x14075bb60` / `0x1407587a0` | `0x18070c330` / `0x1807113a0` / `0x180713120` / `0x18070fd60` | exact / str 0x18705 / exact / exact |
| PopupMenu ctor / popup / onEvent | `0x140753060` / `0x14075d060` / `0x14075abd0` | `0x18070a6d0` / `0x180714620` / `0x180712190` | sz (docs) / sz, head verified / sz |
| UI parameter setter | `0x1400f7c70` | `0x1800e1110` | exact |
| ParamLink bind / onControlNotify / Switch-link / ValueEdit-link / setEngine / hover-select | `0x14011fe30` / `0x140125850` / `0x140121340` / `0x1401213e0` / `0x140130790` / `0x140130150` | `0x18010f720` / `0x180115130` / `0x180110c20` / `0x180110cc0` / `0x180120180` / `0x18011fb40` | sz / exact / exact / exact / exact / exact |
| ParamLinksOwner::bindAll | `0x1401207b0` | `0x1801100a0` | exact |
| FM8FormManager ctor / loadForms / positionForm / showPage | `0x14010c380` / `0x14011a140` / `0x14012b830` / `0x14012fda0` | `0x1800fbb10` / `0x1801099d0` / `0x18011b180` / `0x18011f790` | str / exact / exact / sz |
| FormMain ctor / init / onFormLoaded (menu) / sink / idle | `0x140112d30` / `0x140120340` / `0x14012e310` / `0x140123960` / `0x1401335d0` | `0x1801024c0` / `0x18010fc30` / `0x18011dd00` / `0x180113240` / `0x180122f40` | exact / str / docs / str / str |
| FM matrix builder | `0x14012acb0` | `0x18011a600` | sz |
| WM_PAINT / DIB flush / window class reg / NGL Window::onEvent | `0x1407856a0` / `0x14077e1b0` / `0x140782120` / `0x1400c1e80` | `0x18073c620` / `0x180735240` / `0x180739590` / `0x1800bb240` | str / exact / str / sz |
| FormManager ctor / FormContainer ctor | `0x140752df0` / `0x140752d40` | `0x18070a460` / `0x18070a3b0` | exact |

## 8. Re-verify

```
python tools/q.py exe fn 0x140754bc0     # Form::load: rm slot 1, then +0x130, +0x80
python tools/q.py exe fn 0x14075dcf0     # per-control record: two names, registry, +0xf8, +0x28, +0xb8
python tools/q.py exe fn 0x1407374e0     # registry names
python tools/q.py exe fn 0x140740c50     # SubForm attach: +0x1108 id, +0x1110 child, loadForm
python tools/q.py exe fn 0x14011a140     # FRM id -> member table
python tools/q.py exe fn 0x14075c1a0     # mouse dispatch: +0x94, rect +0x48.., ctrl vt+8
python tools/q.py exe fn 0x14073f550     # Switch click -> notify 0x186a1 {1000, ev, +0x1b4}
python tools/q.py exe fn 0x140738da0     # notify: listener slot 0 (ctrlId=+0x58)
python tools/q.py exe fn 0x1407587a0     # ButtonMenu re-notify, index at data+0x10
python tools/q.py exe fn 0x140130790     # link -> FUN_1400f7c70 -> setParameterByTag
python tools/q.py exe fn 0x140759060     # Form::draw: +0x95, clip, ctrl vt+0xd8
python tools/q.py exe grep 'SetDIBitsToDevice'   # software blit only
```

## 9. UNCONFIRMED

* `DAT_140aaf7c8` is inferred to be the 4-byte `"FRM"` type tag (Ghidra left it undefined; the
  font loader uses `DAT_140aaf748`, pictures use `"PICTURE"`).
* FRM 3 (948x562) has no runtime loader in the exe; assumed unused at runtime.
* Where `FM8ValueEditControl` instances are created (not via the registry).
* Event 0x3eb (1003) semantics (delivered to the focus control; likely wheel).
* EventData fields other than x, y, button, action; modifier keys not mapped.
* Exact emitter line inside `PopupMenu::onEvent` (`FUN_14075abd0`) for the 0x186a1 selection
  notify (the consumer side in `FUN_1407587a0` is confirmed).
* Whether SwitchControl paints its `+0x180` text; Label's text setter.
* Ownership/free of a heap `PopupMenu` shown with `FUN_14075d060` outside a ButtonMenu.
* vst2 rows marked "sz" (same size, partial body match).
