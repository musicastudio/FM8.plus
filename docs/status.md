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
| Mod wheel CC1 rotates Morph X/Y in a circle | pass (traces the full circle, returns to start) |
| Arp Clone to MIDI: audio plays and notes sent to host | pass (17 note-ons, audio 0.073) |
| Arp MIDI only: notes sent, FM8 voices silenced | pass (16 note-ons, audio 0.000) |
| Arp Internal: nothing leaks to MIDI out | pass (0 events) |
| Chunk trailer round-trips per-instance state | pass (FM8 ignores the trailing bytes) |

**VST3, headless** (`tools/vst3host.py`): the proxy loads, forwards the factory unchanged, and the
added event-output bus appears where stock FM8 has none:

```
event out: 1 bus(es)
    [0] 'FM8+ Arp Out' channels=16 busType=0 flags=0x1
```

**Standalone**: builds; `version.dll` exports exactly the four functions FM8.exe imports; reuses the
same core proven under VST2.

**Installer**: `install.ps1` / `uninstall.ps1` round-trip verified in a scratch tree (originals
renamed to `FM8.plus.core`, proxies dropped in, `version.dll` sideloaded; uninstall restores stock).

## Needs a DAW to confirm

- **VST3 arp MIDI out audio path.** The bus registers headless; routing the notes through a real
  host (Cubase, Reaper, Bitwig, Studio One) and hearing/receiving them is unverified. Some hosts
  cache bus layouts and need one plugin rescan after install.
- **VST3 mod-wheel morph** relies on the host delivering param id `0x6d69646b` in
  `inputParameterChanges`; confirmed as the mapping target, not yet observed live.
- **Standalone** arp-out to a WinMM port and the overlay button on FM8's main window are wired but
  only a real run confirms the port selection and the overlay parenting over FM8's GL surface.
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
