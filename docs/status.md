# Status

What is verified, and what still needs a real DAW. Honest accounting, no overclaiming.

## Verified

**Reverse engineering.** All three binaries decompiled (Ghidra 12.1.2), every hook located,
adversarially verified, and cross-ported. Each hooked function resolves at its recorded address with
a byte-identical size across exe / vst2 / vst3 (see `tools/q.py` and the check in the build history).

**VST2, end to end against the installed FM8.dll** (headless host `tools/vst2host.py plus`):

| Check | Result |
|-------|--------|
| Proxy loads, forwards, uniqueID/params unchanged | pass (uniqueID 0x4e696638, 1094 params) |
| Morph Rotate Control on an arbitrary CC | pass (CC 11 traces the full circle; CC1 inert, and the mapped CC is blocked from FM8) |
| Arp Clone to MIDI: audio plays and notes sent to host | pass (17 note-ons, audio 0.073) |
| Arp MIDI only: notes sent, FM8 voices silenced | pass (16 note-ons, audio 0.000) |
| Arp Internal: nothing leaks to MIDI out | pass (0 events) |
| Tempo Override 2x / 0.5x | pass (arp note density 18 -> 37 at 2x, -> 9 at 0.5x) |
| Increase Gain +6 dB / +10 dB | pass (output peak x2.00 and x3.16, matching the dB) |
| Chunk trailer round-trips per-instance state | pass (trailer stripped so FM8 only ever sees its own bytes) |

A flaky crash seen while adding these was a use-after-free in the test host (it freed the VstEvents
buffer before FM8 read it during processReplacing; FM8 stores the pointer), fixed in
`tools/vst2host.py`. It was never in the shim.

**VST3, headless** (`tools/vst3host.py`): the proxy loads, forwards the factory unchanged, and the
added event-output bus appears where stock FM8 has none:

```
event out: 1 bus(es)
    [0] 'FM8+ Arp Out' channels=16 busType=0 flags=0x1
```

**Standalone**: builds; `version.dll` exports exactly the four functions FM8.exe imports; reuses the
same core proven under VST2. Confirmed by launching the real FM8.exe with `version.dll` sideloaded:
our DLL loads, the init runs (the `%APPDATA%\FM8.plus` settings dir is created), the logo shifts and
the drawn **FM8+** "+" renders beside it, and clicking it opens the menu with all four
submenus (Morph Rotate Control, Arpeggiator MIDI out, Tempo Override, Increase Gain). Screenshotted.

**Installer**: `install.ps1` / `uninstall.ps1` round-trip verified in a scratch tree (originals
renamed to `FM8.plus.core`, proxies dropped in, `version.dll` sideloaded; uninstall restores stock).

## Needs a DAW to confirm

- **VST3 arp MIDI out audio path.** The bus registers headless; routing the notes through a real
  host (Cubase, Reaper, Bitwig, Studio One) and hearing/receiving them is unverified. Some hosts
  cache bus layouts and need one plugin rescan after install.
- **VST3 morph on the mapped CC** relies on the host delivering the CC's parameter id (found via
  `IMidiMapping`) in `inputParameterChanges`; confirmed as the mapping target, not yet observed live.
- **Tempo Override and Increase Gain** are verified headless under VST2; the VST3 process-context
  tempo scaling and both features under the standalone still want a listening test in a DAW.
- **Standalone** overlay button and menu confirmed on a real run (see above); the arp-out to a WinMM
  port and the actual audio still need a listening test with a MIDI monitor.
- **VST2 host coverage.** Plugin-to-host MIDI is not routed by every host; where a host drops it,
  Clone/MIDI-only produce nothing downstream (the mode is labelled honestly).

## Known limitations / follow-ups

- The **overlay button** is a Win32 child over the editor. If a host's GL surface repaints over it in
  some DAW, the fallback is the INI plus the native NGL menu (a later upgrade, addresses already
  located in `docs/hooks.md`).
- **Standalone morph** applies via the internal setter using the EditBuffer captured from the arp
  dispatch, which runs every block; if a future FM8 build gates that call, capture the EditBuffer
  from another per-block site instead.
- Arp-out timing carries the in-block sample offset in VST2/VST3; the standalone WinMM path sends at
  the block boundary (one block of jitter), which is fine for loopMIDI or hardware.
