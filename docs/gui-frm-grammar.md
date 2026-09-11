# FM8 GUI form resource (FRM) grammar

Binary layout of the `FRM` resources in the `.rsrc` section of `FM8.exe` (74 resources), recovered from the Ghidra decompilation of the NGL form loader and cross-checked against every shipped resource. Function addresses are VAs in `FM8.exe` (image base `0x140000000`); source paths quoted in assertion strings identify the original NGL files.

Verification: a table-driven parser interpreting the JSON grammar in section 6 parses all 74 FRM resources with no trailing bytes and re-serializes each one byte-for-byte identically (`74/74`, see section 7).

## 1. Stream primitives

All values are little-endian. The stream object (`NI::NGL` resource stream) keeps an endianness flag at `+0x10`; when it is not 1 every multi-byte primitive is byte-swapped. Resources are loaded with the flag set to 1, so the on-disk format is little-endian and no swapping happens.

| Type | Size | Reader | Writer | Notes |
|---|---|---|---|---|
| `u32` | 4 | `FUN_1408ff130`, `FUN_1408fea20` | `FUN_140904e10` | versions, ids, enums, colours |
| `i32` | 4 | `FUN_1408fef80` | `FUN_140904c30` | signed coordinates and sizes |
| `f32` | 4 | `FUN_1408fea20` | `FUN_140904b50` | IEEE single |
| `f64` | 8 | `FUN_1408fea70`, `FUN_1408ff190` | `FUN_140904ba0` | IEEE double |
| `u8` | 1 | `FUN_1408fe890` | `FUN_140904a20` (byte), `FUN_140904c80` (bool) | flags |
| `str` | 4+n | `FUN_1408ff030` | `FUN_140904cb0` | `u32` byte count followed by the bytes, no terminator |
| `rgba` | 4 | as `u32` | as `u32` | `0xAARRGGBB`; default `0xffb0b0b0`; alpha `0xff` is opaque |
| `u16` | 2 | `FUN_1408ff0f0` | | not used by any FRM loader |
| pascal string | 1+n | `FUN_1408fefd0` | | `u8` length; only in the legacy Control v<6 path |

`u32` and `i32` share one encoding; the distinction below follows which reader/writer the code uses.

## 2. Form (file) header

Loader `FUN_14075d6a0` (`form_ngl.cpp`, assert "Invalid Form resource version!"), writer `FUN_140761330`. This is virtual slot 38 (`+0x130`) of `NI::NGL::Form` (vtable `0x140d4b880`).

| # | Field | Type | Stored at | Meaning |
|---|---|---|---|---|
| 1 | `version` | u32 | | must be ≤ 6; the writer emits 6; all 74 files are 6 |
| 2 | `background` | PanelItem | `Form+0x48` | form background: mode / colour / picture (see 3.2). Loaded through the PanelItem vtable slot 1 (`+0x08`) |
| 3 | `width` | i32 | `Form+0x30` | design width; copied to `Form+0x40` (current size) |
| 4 | `height` | i32 | `Form+0x34` | design height; copied to `Form+0x44` |
| 5 | `resizable` | u8 | `Form+0x71` | when set, `Form::setSize` (`FUN_14075ef40`) clamps new sizes to the minimum below |
| 6 | `minWidth`, `minHeight` | i32, i32 | `Form+0x38`, `Form+0x3c` | present only when `resizable != 0` |
| 7 | `name` | str | `Form+0x10` | form name; empty in every shipped file |
| 8 | `count` | i32 | | number of control records that follow; the loader first clears the child list (vtable `+0xd8`) |
| 9 | `controls` | count × control record | | see section 3 |

Example, FRM 5 (`06 00 00 00 | 01 00 00 00 02 00 00 00 b0 b0 b0 ff 01 00 00 00 71 00 00 00 | b4 03 00 00 | 59 00 00 00 | 00 | 00 00 00 00 | 21 00 00 00`): version 6; background PanelItem (version 1, mode 2 = picture, colour `0xffb0b0b0`, picture ref version 1, picture id 113); 948 × 89; not resizable; empty name; 33 controls.

Per-record creation (`FUN_14075dcf0`): reads the header (3.1), creates the control by class name through the registry (`FUN_14072c990`), calls the control's load virtual (slot 31, `+0xf8`) with the stream, maps the record id through the form's id table (`FUN_140765ec0(Form+0x78, Form+0x0c, id)`), calls `setId` (`+0x28`) and `addChild` (Form vtable `+0xb8`). The record id therefore survives unchanged unless the form has an explicit remap entry.

Creation rule: if `name` is non-empty the registry is searched for `name` first and `class` is only a fallback; the resulting control's `+0x28` string is then set to `name`. All registered classes are listed in section 3.4.

## 3. Control record

### 3.1 Record header

Written by `FUN_140761330`, read by `FUN_14075dcf0`.

| Field | Type | Meaning |
|---|---|---|
| `marker` | u32 | always 1 (read and discarded; the writer emits 1 for every child whose id is < `0x80000000`) |
| `id` | u32 | control id (`Control+0x58`) |
| `class` | str | registry class name (`Control+0x08`), e.g. `Label`, `switch.dll` |
| `name` | str | instance name (`Control+0x28`); may itself be a registered class (`FM8ValueEdit`, `XYHandle`, ...) |

### 3.2 Control base (every class)

`NI::NGL::Control::load` = `FUN_140746030` (`control_ngl.cpp`, assert at line 0x248), writer `FUN_1407515e0`. Called first by every class loader.

| Field | Type | Stored at | Meaning |
|---|---|---|---|
| `version` | u32 | | must be ≤ 7; writer emits 7; all 2357 shipped records are 7 |
| `x1`,`y1`,`x2`,`y2` | i32 ×4 | `+0x48..+0x54` via `setRect` (vtable `+0x40`) | bounding rectangle in parent coordinates, exclusive right/bottom |
| `transparent` | u8 | `+0x84` | returned by `isTransparent` (vtable `+0x48`, `FUN_140738a00`). `getOpaqueRect` (`FUN_140735f70`) reports the control rect as opaque only when this is 0. Label, Selector, and ShadeArea override the getter (Label decides from its PanelItem mode/alpha), so the stored value only matters for classes that keep the base getter |
| `tag` | u32 | `+0x88` | integer with no reader anywhere in the NGL core; values 0, 2, 16, 17, 18, 51 occur |
| `layer` | u32 | `+0x80` | draw/z-order layer 0..7; `FUN_14074ae40` clamps ≥8 to 6 and asks the parent to re-sort (`Form` vtable `+0xe0` = `FUN_140758c60`, remove + re-insert sorted). Labels use 0..5, switches 3, ShadeArea 4, XYHandle 6 |
| `help` | str | `+0x60` | tooltip/help text (only `"Output level"` on one selector; empty otherwise). Present when version ≥ 7 |

Versions < 6 are a legacy layout (pascal string, then up to four u32 depending on version) that no shipped file uses; the writer only emits 7.

### 3.3 Shared nested items

**ResourceRef** (`NI::NGL::DETAIL::ResourceRef<T,Manager>::load` = `FUN_140745fc0`, used for pictures and fonts): `version` u32 (written 1, never checked), `id` i32 (`+0x08`; 0 = none; non-zero ids get `DAT_141201b68` added at load, the resource id base which is 0 in FM8). Picture ids index the `PictureManager`, font ids the `FontManager`.

**PanelItem** (`panelitem_ngl.cpp`, load `FUN_1407467b0`, save `FUN_1407519d0`, vtable `0x140d46108`; 0x28 bytes): `version` u32 ≤ 1; `mode` u32 (`+0x20`); `colour` rgba (`+0x24`); `picture` ResourceRef (`+0x08`). Draw (`FUN_14072ff50`): mode 0 draws nothing, 1 fills the rect with `colour`, 2 blits picture frame *n* (frame index supplied by the owner: Label `frame`, Switch state, ...), 3 blits the picture with the frame index clamped to the frame count (used for backgrounds). Label's `isTransparent` returns true for mode 0, `alpha != 0xff` for mode 1, and the picture's alpha flag for 2/3.

**TextItem** (`textitem_ngl.cpp`, load `FUN_140747ec0`, save `FUN_140752160`; 0x78 bytes): `version` u32 ≤ 2; `font` ResourceRef<Font>; `hAlign` u32 (`+0x20`: 0 left, 1 centre, 2 right, `FUN_1407320c0`); `vAlign` u32 (`+0x24`: 0 top, 1 middle, 2 bottom, `FUN_140730160`); `text` str (`+0x28`). The text is a key: it is decoded (`FUN_1408ead00`, mode 6 for v2, mode 3 for v1), looked up in the global translation map (`FUN_1408ee990`, inserted if missing) and literal `\n` pairs are turned into line breaks. 48 shipped TextItems are v1, 2000 are v2; the field layout is identical.

**TextPanelItem** (`textpanelitem_ngl.cpp`, load `FUN_140748330`, save `FUN_140752210`, ctor `FUN_140726c20`; 0xb0 bytes): `version` u32 ≤ 2; `panel` PanelItem (`+0x00`); `text` TextItem (`+0x28`); then for v2: `marginLeft` (`+0xa0`, added to x1), `marginRight` (`+0xa4`, subtracted from x2), `marginTop` (`+0xa8`), `marginBottom` (`+0xac`) as u32 (`FUN_1407305a0`). All shipped instances are v2.

**SelectableTextPanelItem** (`selectabletextpanelitem_ngl.cpp`, load `FUN_140746b70`, save `FUN_140751ce0`): a TextPanelItem followed by `version` u32 ≤ 1 and, for v1: `selPicture` ResourceRef (`+0xb0`), `selFont` ResourceRef (`+0xc8`), `selColour` rgba (`+0xe0`), `selColour2` rgba (`+0xe4`), `flag_e8` u8, `flag_e9` u8 (both select alternate picture frames in the row painter `FUN_1407319f0`).

**ScrollablePane** (`NI::NGL::DETAIL::ScrollablePane`, ctor `FUN_140725e30`, load `FUN_140746970`, save `FUN_140751ae0`): `version` u32 (must be 1); nine PanelItems `part0..part8` (the scrollbar/track/button parts at `+0x10, +0x48, ... +0x1d0`, stride 0x38); `barWidth` i32 (written as `+0xe8 - +0xe0`, applied through `FUN_14074c800`); 15 i32 layout offsets (`+0x218,+0x21c,+0x220,+0x208,+0x20c,+0x210,+0x214,+0x384,+0x388,+0x38c,+0x390,+0x394,+0x398,+0x39c,+0x3a0`, scrollbar margins consumed by the layout routine `FUN_14071efb0`); four u32 modes (`+0x308,+0x30c,+0x310,+0x314`, values 0/1/2 controlling scrollbar visibility). Only the 12 list and tree controls carry one.

### 3.4 Class registry

Core registry (`FUN_1407374e0`, `init_ngl.cpp`), name → creator: `Generic`, `Label`, `LevelMonitor`/`levelMonitor.dll`, `List2`, `ObjectBoxControl`, `Scope`/`scope.dll`, `Selector`/`selector.dll`, `ShadeArea`/`Shade Area`, `StackedSubForm`, `SubForm`, `Switch`/`switch.dll`, `TextEdit`, `Tree View`, `ValueEdit`. Application registrations through `FUN_14072a0d0`: `ButtonMenu` (`FUN_140755540`), `ButtonMenuControl2`, `FM8ValueEdit`, `FM8Envelope`, `MorphSelector`, `ListControl2Helper`, `MIDI_CCList`, `Arpeggiator`, `EffectRack`, `EffectList`, `XYHandle`, `XYHandleMorph`, `SoundAttributesSFC`, `SoundBrowserSFC`, `BrowserTree`, `DelayedTextControl`, `AttributesListControl`, `FixColumnListControl`, `FontChangeSwitch`, `MoverControl`, `BrowserMainMover`, `SND::ProgramListControl`, `SND::ProgramListSubForm`, `DetailSearchPage`, `SelectAttributesListControl`.

The `.dll` aliases and `Shade Area` map to the same creator as the plain name, so they share the loader. Every application class used in the 74 forms keeps its base class's load virtual (checked by reading slot 31 of each vtable): `FM8ValueEdit`→ValueEdit; `MorphSelector`, `Arpeggiator`, `EffectRack`, `SoundAttributesSFC`, `SoundBrowserSFC`, `BrowserMainMover`, `SND::ProgramListSubForm`, `DetailSearchPage`→SubForm; `XYHandle`, `XYHandleMorph`, `EffectList` (thunk `FUN_140169b60`)→Control only; `FontChangeSwitch`, `MoverControl`→Switch; `DelayedTextControl`→TextEdit; `AttributesListControl`, `SelectAttributesListControl`, `FixColumnListControl`, `ListControl2Helper`, `MIDI_CCList`, `SND::ProgramListControl`→List2; `BrowserTree`→Tree. Two records have class `List`, which is registered nowhere; they are created through their name `ListControl2Helper`.

Load virtual = vtable slot 31 (`+0xf8`), save = slot 32 (`+0x100`). Slot 3 (`+0x18`) is the event handler, not the loader.

| Class | vtable | load | save | max version | shipped |
|---|---|---|---|---|---|
| Control (Generic) | `0x140d45fd0` / `0x140d48430` | `FUN_140746030` | `FUN_1407515e0` | 7 | 7 |
| Label | `0x140d48568` | `FUN_140746230` | `FUN_140751690` | 2 | 2 |
| Switch | `0x140d461a8` | `FUN_140747510` | `FUN_140752030` | 11 | 11 |
| SubForm / StackedSubForm | `0x140d462e8` / `0x140d49d60` | `FUN_1407473d0` | `FUN_140751fb0` | 1 | 1 |
| ValueEdit | `0x140d46c38` | `FUN_140748740` | `FUN_140752420` | 5 | 5 |
| Selector | `0x140d49ab0` | `FUN_140746c60` | `FUN_140751dc0` | 12 | 12 |
| TextEdit | `0x140d490b0` | `FUN_1407479d0` | `FUN_1407518e0` | 6 | 6 |
| ButtonMenu | `0x140d4c658` | `FUN_14075d2d0` | `FUN_1407612b0` | 2 | 2 |
| PopupMenu (inside ButtonMenu) | `0x140d4c3b8` | `FUN_14075d8f0` | `FUN_1407614a0` | 1 | 1 |
| Scope | `0x140d49978` | `FUN_140746840` | `FUN_140751a50` | 4 | 4 |
| LevelMonitor | `0x140d486a0` | `FUN_1407462e0` | `FUN_140751710` | 3 | 3 |
| ShadeArea | `0x140d49c28` | `FUN_140747330` | `FUN_140751f40` | 1 | 1 |
| List2 (Control sub-object at `+0x3b0`) | `0x140d48b00` | `FUN_140746460` | `FUN_1407517a0` | 12 | 9, 12 |
| Tree | `0x140d4a000` | `FUN_140748430` | `FUN_140752340` | 6 | 6 |

## 4. Class-specific fields

Each class loader runs after the Control base and starts with its own `version` u32. Offsets are relative to the control object. "Shipped" counts come from the 74 resources.

### Generic (8 records: XYHandle, XYHandleMorph, EffectList)
No fields beyond the Control base.

### Label (1173), `labelcontrol_ngl.cpp`
| Field | Type | Stored at | Meaning |
|---|---|---|---|
| `version` | u32 | | ≤ 2 |
| `item` | TextPanelItem | `+0xc8` | background + caption |
| `frame` | u32 | `+0x17c` | picture frame index handed to the item draw (`FUN_14073a410`); v ≥ 2. Always 0 in shipped forms |

### Switch (476, classes `Switch`, `switch.dll`), `switchcontrol_ngl.cpp`
| Field | Type | Stored at | Meaning |
|---|---|---|---|
| `version` | u32 | | ≤ 11 |
| `pressedOffsetX`, `pressedOffsetY` | i32, i32 | `+0x1ac`, `+0x1b0` | caption offset applied while the button is held (`FUN_14073b620`); v11 only |
| `item` | TextPanelItem | `+0xc8` | picture strip + caption. Frames per state = `(frames - disabledFrame) / ((pressedFrames ? 2 : 1) + hoverFrames)` (`FUN_140737ed0`) |
| `value` | u32 | `+0x1b4` | current state, also the base frame index; 0 in every shipped record |
| `toggle` | u8 | `+0x17b` | a click flips `value` between 0 and 1 |
| `hoverFrames` | u8 | `+0x178` | strip has an extra mouse-over frame set |
| `pressedFrames` | u8 | `+0x179` | strip has a pressed frame set (doubles the frame sets) |
| `disabledFrame` | u8 | `+0x17a` | last frame is the disabled look (used when `+0x94` enabled flag is 0) |
| `primaryButtonOnly` | u8 | `+0x17c` | ignore clicks from mouse buttons other than 1 (`FUN_14073f550`) |
| `mode` | u32 | `+0x1a0` | 0/1 send value on click; 2 auto-repeat (registers a timer via `FUN_140754470`); 3 send on mouse-down only; 4 send on release path only |
| `repeatInterval` | u32 | `+0x1a8` | timer period handed to the form when `mode == 2` (4 in shipped forms) |
| `repeatDelay` | u32 | `+0x1a4` | ticks before repeating (copied to `+0x1c4`); always 1 |
| `keyboard` | u8 | `+0x17d` | handles keyboard event 1000 (sub-type 3) |
| `text` | str | `+0x180` | v ≥ 10; always empty |

### SubForm / StackedSubForm (59), `subformcontrol_ngl.cpp`
| Field | Type | Stored at | Meaning |
|---|---|---|---|
| `version` | u32 | | ≤ 1 |
| `formName` | str | `+0x1118` | optional form name (`MasterVolumeForm_KPI` once) |
| `formId` | u32 | `+0x1108` | FRM resource id of the embedded form (FRM 3 embeds 5, FRM 10 embeds 38/43/45/49) |

### ValueEdit (291, also `FM8ValueEdit`), `valueeditcontrol_ngl.cpp`
| Field | Type | Stored at | Meaning |
|---|---|---|---|
| `version` | u32 | | ≤ 5 |
| `item` | TextPanelItem | `+0xc8` | background + text style |
| `editable` | u8 | `+0x1cf` | double-click opens the text editor; when 0 a double-click resets the value (`FUN_1407438a0`) |
| `zeroSpecial` | u8 | `+0x234` | the formatter treats value 0 specially (`FUN_14072bae0`) |
| `dragScale` | i32 | `+0x230` | 2 = logarithmic drag, otherwise linear; always 1 in shipped forms |
| `dragSensitivity` | f32 | `+0x238` | mouse-drag scale (0.2 typical) |
| `flag_1ce` | u8 | `+0x1ce` | no reader found; 0 except one record |
| `valueType` | u32 | `+0x1d8` | 0 float, 1 int, 2 int 0..127, 3 int 0..16, 4 double (`FUN_14074e110`) |
| `min`, `max` | typed | `+0x1f0`, `+0x200` | both f32 when `valueType == 0`, i32 when 1..3, f64 when 4; absent for other values |
| `format` | str | `+0x178` | printf format for display, e.g. `%3.0f`, `%8.3f` (`FUN_14074da40` scans it for `%`) |
| `flag_1d0` | u8 | `+0x1d0` | v ≥ 5; selects frame+1 in the painter; always 0 |

### Selector (193, classes `Selector`, `selector.dll`), `selectorcontrol_ngl.cpp`
| Field | Type | Stored at | Meaning |
|---|---|---|---|
| `version` | u32 | | ≤ 12 |
| `picture` | ResourceRef | `+0xf0` | knob/slider frame strip (`+0xf8` id) |
| `item` | PanelItem | `+0xc8` | background; v ≥ 11 |
| `min`, `max` | f32, f32 | `+0x10c`, `+0x110` | value range |
| `steps` | u32 | `+0x114` | value = frame × (max − min) / steps + min (`FUN_140736e50`); keyboard step size |
| `hasPicture` | u8 | `+0x118` | size the control to the picture and use the picture alpha for transparency (`FUN_140737e20`, `FUN_140738a70`) |
| `vertical` | u8 | `+0x119` | drag and position measured along y (`FUN_14072b320`, `FUN_14072b250`) |
| `keyboard` | u8 | `+0x11a` | cursor keys/home/end change the value while focused |
| `disabledFrame` | u8 | `+0x11b` | last frame is the disabled look |
| `dragMode` | u32 | `+0x13c` | 0 relative linear drag, 1 absolute handle position, 2 rotary (angle around `centreX/Y`, `FUN_14072b070`) |
| `sensitivity` | f32 | `+0x140` | relative drag scale |
| `handleMargin` | u32 | `+0x144` | pixels excluded at both ends in absolute mode |
| `centreX`, `centreY` | i32, i32 | `+0x148`, `+0x14c` | rotary centre offset; always 2,2 |
| `flag_150` | u8 | `+0x150` | no reader found; always 0 |
| `fineFactor` | f32 | `+0x154` | sensitivity multiplier while the fine-control modifier (bit 28) is held; v ≥ 12 |
| `defaultValue` | f32 | `+0x15c` | reset target; v ≥ 12 |
| `resetMode` | u32 | `+0x158` | 0 double-click resets to 0 when min < 0 < max, 2 double-click resets to `defaultValue`, 3 modifier-click, 4 button-2 click (`FUN_140743150`); v ≥ 12 |

### TextEdit (55, also `DelayedTextControl`), `texteditcontrol_ngl.cpp`
| Field | Type | Stored at | Meaning |
|---|---|---|---|
| `version` | u32 | | ≤ 6 |
| `item` | TextPanelItem | `+0x498` | `TextEditControl::TextPanelItem` (same loader) |
| `multiline` | u8 | `+0x554` | multi-line editing/painting (`FUN_14073b7d0`) |
| `readOnly` | u8 | `+0x550` | stored as int; blocks editing |
| `flag_555` | u8 | `+0x555` | disables selection painting; the loader clears `multiline` when both are set |
| `flag_5a8` | u8 | `+0x5a8` | only consulted with `multiline` and `readOnly` |
| `disabledFrame` | u8 | `+0x557` | v ≥ 6; last picture frame when disabled |

### ButtonMenu (26), `buttonmenu_ngl.cpp`
A Switch record (all Switch fields, version 11) followed by:

| Field | Type | Stored at | Meaning |
|---|---|---|---|
| `version` | u32 | | ≤ 2 |
| `captionFromSelection` | u8 | `+0x3bc` | v ≥ 2; the button caption follows the selected item (`FUN_1407610e0`) |
| `menu` | PopupMenu | `+0x1e0` | embedded `NI::NGL::PopupMenu`, loaded through its own vtable `+0xf8` |

PopupMenu (`popupmenu_ngl.cpp`): a complete **Control base** record (version 7, rect, transparent, tag, layer, help; rect is 0,0,0,0 and layer 7 in every shipped menu), then `version` u32 ≤ 1, `count` u32, and `count` items. Item: `separator` u8; when 0 it is followed by `label` str (translation key) and `value` i32 (stored at entry `+0x60`; the writer emits 0 when the entry's `+0x68` is non-zero). Only 391 non-separator items ship.

### Scope (19, classes `Scope`, `scope.dll`), `scopecontrol_ngl.cpp`
| Field | Type | Stored at | Meaning |
|---|---|---|---|
| `version` | u32 | | ≤ 4 |
| `lineColour` | rgba | `+0xf0` | trace colour (`FUN_14073af10`) |
| `lineColour2` | rgba | `+0xf4` | second colour for `drawMode` 1/2 |
| `drawMode` | u32 | `+0x100` | 0 single-colour trace, 1/2 two-colour (`FUN_1407327a0`) |
| `item` | PanelItem | `+0xc8` | background |

### LevelMonitor (14, classes `LevelMonitor`, `levelMonitor.dll`), `levelmonitorcontrol_ngl.cpp`
| Field | Type | Stored at | Meaning |
|---|---|---|---|
| `version` | u32 | | ≤ 3 |
| `picture` | ResourceRef | `+0xc8` | frame strip (`+0xd0` id) |
| `maxValue` | u32 | `+0xe8` | value range: frame = levelFrames × value / maxValue (`FUN_140750410`); 131/142/255 |
| `peakFrames` | u32 | `+0xe4` | extra peak-hold frames after the level frames (0 = none) |

### ShadeArea (3, classes `ShadeArea`, `Shade Area`), `shadeareacontrol_ngl.cpp`
| Field | Type | Stored at | Meaning |
|---|---|---|---|
| `version` | u32 | | ≤ 1 |
| `colour` | rgba | `+0xc8` | fill colour (`FUN_14073b3f0`); default `0xff800000` |

### List2 (13, classes `List2`, `List`; names AttributesListControl etc.), `ListControl2::load`
The record continues with `version` u32 (9..12 accepted). Offsets are relative to the Control sub-object at outer `+0x3b0` (the outer object is a `ListControlDefImpl`).

Version 12 (11 records):
| Field | Type | Stored at | Meaning |
|---|---|---|---|
| `headerItem` | TextPanelItem | `+0x11f8` | column header style |
| `flag_13da` | u8 | `+0x13da` | |
| `rowItem` | SelectableTextPanelItem | `+0x12a8` | row style incl. selection picture/font/colours |
| `flag_13d8` | u8 | `+0x13d8` | |
| `selectionColour` | rgba | `+0x13d4` | e.g. `0x55000000` |
| `panel` | PanelItem | `+0x13a0` | list background |
| `i32_13c8`, `i32_11a0`, `i32_13cc` | i32 | | layout ints (1/2, row width 100..246, 0..3) |
| `flag_13d9` | u8 | `+0x13d9` | |
| `i32_13e0`, `i32_13d0` | i32 | | row height (14..18), 0..2 |
| `pictureA`, `pictureB` | u32 | `+0x13e8`, `+0x1400` | picture resource ids resolved in `FUN_1407397a0` |
| `i32_13e4` | i32 | | 1 or 2 |
| `pane` | ScrollablePane | outer `+0x00` | scrollbar layout |

Versions 9 and 10 (2 records, both named `ListControl2Helper` with class `List`): `headerItem` TextPanelItem; `rowItem` TextPanelItem (plain; v11 reads it as SelectableTextPanelItem); `i32_13c8`, `i32_13cc`, `i32_13d0`, `i32_13e4` i32; `colour_13c4` rgba; `partsColour` rgba (applied to all nine ScrollablePane parts through `FUN_14074c6e0`); `colour_1388` rgba; two ignored u32; `flag_13d8` u8; `selectionColour` rgba; `pictureA`, `pictureB` u32; `flag_set270` u8 (passed to vtable `+0x270`); `flag_1390` u8. No ScrollablePane block.

### Tree (1, class `Tree View`, name `BrowserTree`), `treecontrol_ngl.cpp`
`version` u32 ≤ 6, then for v ≥ 2: `flag_659` u8, `flags_480` i32 (or-ed flag word), `indent` i32 (`+0x484`), `indentPerLevel` i32 (`+0x490`, multiplied by node depth), `resource_470` u32 (picture id, 6043); v ≥ 5: `resource_5c0` u32, `rowItem` SelectableTextPanelItem (`+0x4a0`), `panel` PanelItem (`+0x598`); v ≥ 6: `pane` ScrollablePane (`+0xc8`). Versions 2..4 use a legacy tail (colour, row height, font id) that never ships.

## 5. Version branches

Every loader asserts `version <= max` (`FUN_1408ee5b0` with the source path) and the writer always emits the maximum, so a recompiler should emit the maximum versions. The shipped forms use: Form 6; Control 7; PanelItem 1; TextItem 1 (48 items) and 2 (2000); TextPanelItem 2; SelectableTextPanelItem 1; ScrollablePane 1; Label 2; Switch 11; SubForm 1; ValueEdit 5; Selector 12; TextEdit 6; ButtonMenu 2; PopupMenu 1; Scope 4; LevelMonitor 3; ShadeArea 1; List2 9 (2 records) and 12 (11); Tree 6.

Branches in the loaders that shipped files do not take (kept here so a decompiler can reject or support them deliberately):

- **Form** v1: `u32` picture id only (`FUN_14074bb50`). v2: PanelItem only. v3-4: + width/height. v5: + resizable/minSize. v6: + name. The record loader receives `version < 4` as a flag it never uses.
- **Control** v<6: pascal string; v≥2 `u32`; v≥3 `u32`→`transparent`; v<4 `u32`; v≥5 `u32`→`tag`. v6: rect, transparent, tag, layer without `help`.
- **TextItem** v1: text decoded with mode 3 instead of 6. **TextPanelItem** v1: no margins.
- **Label** v1: no `frame`.
- **Switch** v5-8: no TextPanelItem; a raw picture id follows `value` and is assigned with `FUN_140131100`, the PanelItem mode is forced to 2; flags are u32 in v5/6 and u8 from v7; v9/10 lack the pressed offsets; v10 adds `text`; v11 adds the two offsets in front of the item.
- **ValueEdit** v1: TextPanelItem only. v2: item, f32 min, f32 max, format, editable, zeroSpecial, dragScale, dragSensitivity, flag_1ce. v3: v2 + valueType. v4: item, editable, zeroSpecial, dragScale, dragSensitivity, flag_1ce, valueType, typed range, format. v5: v4 + flag_1d0.
- **Selector** v7: all-u32 legacy record. v8/9: no picture ResourceRef, mode encoded differently. v10: ResourceRef then fields through `flag_150`. v11: + PanelItem. v12: + fineFactor, defaultValue, resetMode.
- **TextEdit** v1: u32, str, u8, u8. v2: u32, u32 ×3, str, u8, u8. v3: u32, u32, str, u8, u8. v4: item only. v5: item + 4 flags. v6: + disabledFrame.
- **ButtonMenu** v1: no `captionFromSelection`.
- **Scope** v1: nothing. v2: lineColour + PanelItem. v3: lineColour, u8 drawMode, PanelItem. v4: as documented.
- **LevelMonitor** v2: u32, u32 picture id, u32 ×3, u32 peakFrames, u32 maxValue, u32 (note the reversed order).
- **List2** v9/10 and v11 (row item type differs) as above; v12 adds the ScrollablePane and the byte flags.
- **Tree** v2-4 legacy tail; v5 without ScrollablePane.

## 6. Machine-readable grammar

Types: `i32 u32 u8 f32 f64 str rgba`; a bare name is a nested structure; `if:F:S` includes structure `S` when earlier field `F` is non-zero; `ifzero:F:S` when it is zero; `list:F:S` repeats `S` `F` times; `switch:F:S` picks `variants[S][value of F]` (for `List2` the selector is the variant's own leading `version`); `controls` is `count` control records, each being `ControlHeader` + `ControlBase` + the structure named by `classes[name if name in classes else class]`.

```json
{
  "types": ["i32", "u32", "u8", "f32", "f64", "str", "rgba", "<Struct>", "if:<field>:<Struct>", "ifzero:<field>:<Struct>", "list:<field>:<Struct>", "switch:<field>:<Struct>", "controls"],
  "grammar": {
    "Form": [
      ["version", "u32"],
      ["background", "PanelItem"],
      ["width", "i32"],
      ["height", "i32"],
      ["resizable", "u8"],
      ["minSize", "if:resizable:MinSize"],
      ["name", "str"],
      ["count", "i32"],
      ["controls", "controls"]
    ],
    "MinSize": [
      ["minWidth", "i32"],
      ["minHeight", "i32"]
    ],
    "ControlHeader": [
      ["marker", "u32"],
      ["id", "u32"],
      ["class", "str"],
      ["name", "str"]
    ],
    "ControlBase": [
      ["version", "u32"],
      ["x1", "i32"],
      ["y1", "i32"],
      ["x2", "i32"],
      ["y2", "i32"],
      ["transparent", "u8"],
      ["tag", "u32"],
      ["layer", "u32"],
      ["help", "str"]
    ],
    "ResourceRef": [
      ["version", "u32"],
      ["id", "i32"]
    ],
    "PanelItem": [
      ["version", "u32"],
      ["mode", "u32"],
      ["colour", "rgba"],
      ["picture", "ResourceRef"]
    ],
    "TextItem": [
      ["version", "u32"],
      ["font", "ResourceRef"],
      ["hAlign", "u32"],
      ["vAlign", "u32"],
      ["text", "str"]
    ],
    "TextPanelItem": [
      ["version", "u32"],
      ["panel", "PanelItem"],
      ["text", "TextItem"],
      ["marginLeft", "u32"],
      ["marginRight", "u32"],
      ["marginTop", "u32"],
      ["marginBottom", "u32"]
    ],
    "SelectableTextPanelItem": [
      ["base", "TextPanelItem"],
      ["version", "u32"],
      ["selPicture", "ResourceRef"],
      ["selFont", "ResourceRef"],
      ["selColour", "rgba"],
      ["selColour2", "rgba"],
      ["flag_e8", "u8"],
      ["flag_e9", "u8"]
    ],
    "ScrollablePane": [
      ["version", "u32"],
      ["part0", "PanelItem"],
      ["part1", "PanelItem"],
      ["part2", "PanelItem"],
      ["part3", "PanelItem"],
      ["part4", "PanelItem"],
      ["part5", "PanelItem"],
      ["part6", "PanelItem"],
      ["part7", "PanelItem"],
      ["part8", "PanelItem"],
      ["barWidth", "i32"],
      ["i32_218", "i32"],
      ["i32_21c", "i32"],
      ["i32_220", "i32"],
      ["i32_208", "i32"],
      ["i32_20c", "i32"],
      ["i32_210", "i32"],
      ["i32_214", "i32"],
      ["i32_384", "i32"],
      ["i32_388", "i32"],
      ["i32_38c", "i32"],
      ["i32_390", "i32"],
      ["i32_394", "i32"],
      ["i32_398", "i32"],
      ["i32_39c", "i32"],
      ["i32_3a0", "i32"],
      ["mode_308", "u32"],
      ["mode_30c", "u32"],
      ["mode_310", "u32"],
      ["mode_314", "u32"]
    ],
    "MenuItem": [
      ["separator", "u8"],
      ["entry", "ifzero:separator:MenuEntry"]
    ],
    "MenuEntry": [
      ["label", "str"],
      ["value", "i32"]
    ],
    "ValueRange_f32": [
      ["min", "f32"],
      ["max", "f32"]
    ],
    "ValueRange_i32": [
      ["min", "i32"],
      ["max", "i32"]
    ],
    "ValueRange_f64": [
      ["min", "f64"],
      ["max", "f64"]
    ],
    "Generic": [],
    "Label": [
      ["version", "u32"],
      ["item", "TextPanelItem"],
      ["frame", "u32"]
    ],
    "Switch": [
      ["version", "u32"],
      ["pressedOffsetX", "i32"],
      ["pressedOffsetY", "i32"],
      ["item", "TextPanelItem"],
      ["value", "u32"],
      ["toggle", "u8"],
      ["hoverFrames", "u8"],
      ["pressedFrames", "u8"],
      ["disabledFrame", "u8"],
      ["primaryButtonOnly", "u8"],
      ["mode", "u32"],
      ["repeatInterval", "u32"],
      ["repeatDelay", "u32"],
      ["keyboard", "u8"],
      ["text", "str"]
    ],
    "SubForm": [
      ["version", "u32"],
      ["formName", "str"],
      ["formId", "u32"]
    ],
    "ValueEdit": [
      ["version", "u32"],
      ["item", "TextPanelItem"],
      ["editable", "u8"],
      ["zeroSpecial", "u8"],
      ["dragScale", "i32"],
      ["dragSensitivity", "f32"],
      ["flag_1ce", "u8"],
      ["valueType", "u32"],
      ["range", "switch:valueType:ValueRange"],
      ["format", "str"],
      ["flag_1d0", "u8"]
    ],
    "Selector": [
      ["version", "u32"],
      ["picture", "ResourceRef"],
      ["item", "PanelItem"],
      ["min", "f32"],
      ["max", "f32"],
      ["steps", "u32"],
      ["hasPicture", "u8"],
      ["vertical", "u8"],
      ["keyboard", "u8"],
      ["disabledFrame", "u8"],
      ["dragMode", "u32"],
      ["sensitivity", "f32"],
      ["handleMargin", "u32"],
      ["centreX", "i32"],
      ["centreY", "i32"],
      ["flag_150", "u8"],
      ["fineFactor", "f32"],
      ["defaultValue", "f32"],
      ["resetMode", "u32"]
    ],
    "TextEdit": [
      ["version", "u32"],
      ["item", "TextPanelItem"],
      ["multiline", "u8"],
      ["readOnly", "u8"],
      ["flag_555", "u8"],
      ["flag_5a8", "u8"],
      ["disabledFrame", "u8"]
    ],
    "ButtonMenu": [
      ["switch", "Switch"],
      ["version", "u32"],
      ["captionFromSelection", "u8"],
      ["menu", "PopupMenu"]
    ],
    "PopupMenu": [
      ["control", "ControlBase"],
      ["version", "u32"],
      ["count", "u32"],
      ["items", "list:count:MenuItem"]
    ],
    "Scope": [
      ["version", "u32"],
      ["lineColour", "rgba"],
      ["lineColour2", "rgba"],
      ["drawMode", "u32"],
      ["item", "PanelItem"]
    ],
    "LevelMonitor": [
      ["version", "u32"],
      ["picture", "ResourceRef"],
      ["maxValue", "u32"],
      ["peakFrames", "u32"]
    ],
    "ShadeArea": [
      ["version", "u32"],
      ["colour", "rgba"]
    ],
    "List2": [
      ["body", "switch:version:List2"]
    ],
    "List2_v12": [
      ["version", "u32"],
      ["headerItem", "TextPanelItem"],
      ["flag_13da", "u8"],
      ["rowItem", "SelectableTextPanelItem"],
      ["flag_13d8", "u8"],
      ["selectionColour", "rgba"],
      ["panel", "PanelItem"],
      ["i32_13c8", "i32"],
      ["i32_11a0", "i32"],
      ["i32_13cc", "i32"],
      ["flag_13d9", "u8"],
      ["i32_13e0", "i32"],
      ["i32_13d0", "i32"],
      ["pictureA", "u32"],
      ["pictureB", "u32"],
      ["i32_13e4", "i32"],
      ["pane", "ScrollablePane"]
    ],
    "List2_v9": [
      ["version", "u32"],
      ["headerItem", "TextPanelItem"],
      ["rowItem", "TextPanelItem"],
      ["i32_13c8", "i32"],
      ["i32_13cc", "i32"],
      ["i32_13d0", "i32"],
      ["i32_13e4", "i32"],
      ["colour_13c4", "rgba"],
      ["partsColour", "rgba"],
      ["colour_1388", "rgba"],
      ["ignoredA", "u32"],
      ["ignoredB", "u32"],
      ["flag_13d8", "u8"],
      ["selectionColour", "rgba"],
      ["pictureA", "u32"],
      ["pictureB", "u32"],
      ["flag_set270", "u8"],
      ["flag_1390", "u8"]
    ],
    "Tree": [
      ["version", "u32"],
      ["flag_659", "u8"],
      ["flags_480", "i32"],
      ["indent", "i32"],
      ["indentPerLevel", "i32"],
      ["resource_470", "u32"],
      ["resource_5c0", "u32"],
      ["rowItem", "SelectableTextPanelItem"],
      ["panel", "PanelItem"],
      ["pane", "ScrollablePane"]
    ]
  },
  "variants": {
      "ValueRange": {
          "0": "ValueRange_f32",
          "1": "ValueRange_i32",
          "2": "ValueRange_i32",
          "3": "ValueRange_i32",
          "4": "ValueRange_f64"
      },
      "List2": {
          "9": "List2_v9",
          "10": "List2_v9",
          "12": "List2_v12"
      }
  },
  "classes": {
      "Generic": "Generic",
      "Label": "Label",
      "Switch": "Switch",
      "switch.dll": "Switch",
      "SubForm": "SubForm",
      "StackedSubForm": "SubForm",
      "ValueEdit": "ValueEdit",
      "Selector": "Selector",
      "selector.dll": "Selector",
      "TextEdit": "TextEdit",
      "ButtonMenu": "ButtonMenu",
      "Scope": "Scope",
      "scope.dll": "Scope",
      "LevelMonitor": "LevelMonitor",
      "levelMonitor.dll": "LevelMonitor",
      "List2": "List2",
      "ShadeArea": "ShadeArea",
      "Shade Area": "ShadeArea",
      "Tree View": "Tree",
      "FM8ValueEdit": "ValueEdit",
      "MorphSelector": "SubForm",
      "Arpeggiator": "SubForm",
      "EffectRack": "SubForm",
      "EffectList": "Generic",
      "XYHandle": "Generic",
      "XYHandleMorph": "Generic",
      "SoundAttributesSFC": "SubForm",
      "SoundBrowserSFC": "SubForm",
      "BrowserTree": "Tree",
      "DelayedTextControl": "TextEdit",
      "AttributesListControl": "List2",
      "SelectAttributesListControl": "List2",
      "FixColumnListControl": "List2",
      "ListControl2Helper": "List2",
      "MIDI_CCList": "List2",
      "SND::ProgramListControl": "List2",
      "FontChangeSwitch": "Switch",
      "MoverControl": "Switch",
      "BrowserMainMover": "SubForm",
      "SND::ProgramListSubForm": "SubForm",
      "DetailSearchPage": "SubForm",
      "FM8Envelope": "Generic",
      "ButtonMenuControl2": "ButtonMenu",
      "List": "List2"
  }
}
```

## 7. Round-trip proof

`frm2.py` (scratch, `...\scratchpad\agentA\frm2.py`) interprets the JSON above: `parse()` walks the grammar to build a tree of decoded values, `serialize()` re-encodes the tree from the decoded values (not from the original bytes). Result over `rsrc\FRM\*.bin`:

```
round-trip byte-identical: 74/74
```

Every file is consumed exactly (no trailing bytes) and re-emitted identically, including FRM 13 and 57 (List2 version 9), FRM 32 and 34 (`Shade Area`), and FRM 6002 (`Tree View`/`BrowserTree`). Record census: Label 1173, switch.dll 312, ValueEdit 291, Switch 164, selector.dll 145, SubForm 59, TextEdit 55, Selector 48, ButtonMenu 26, scope.dll 17, levelMonitor.dll 11, List2 11, Generic 8, LevelMonitor 3, List 2, Shade Area 2, Scope 2, ShadeArea 1, Tree View 1 (2357 controls, 26 embedded PopupMenus with 391 items).
