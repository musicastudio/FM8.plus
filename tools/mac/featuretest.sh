#!/bin/sh
# The Mac feature battery, every format: arp MIDI out (Clone, MIDI only), morph, AU state, auval,
# the audio-thread CPU against stock FM8, and the "+" menu. Prints each probe's RESULT line.
#     sh tools/mac/featuretest.sh            from ~/fm8plus, in an ssh session (AU runs go through ingui.sh)
# Rewrites /Users/Shared/FM8.plus/FM8.plus.ini as it goes and leaves it as [FM8.plus].
# Reading it: arp mode 2 (MIDI only) passes with note-ons out and audio peak 0.000, whatever the VST2
# probe's verdict says (it also wants audio); the VST3 morph line reads the controller's cache, which
# does not follow the engine (FM8PLUS_TRACE=1 shows the engine moving).
cd "$(dirname "$0")/../.." || exit 1
P=~/Library/Audio/Plug-Ins
INI=/Users/Shared/FM8.plus/FM8.plus.ini
B=./build-mac
ini() { printf '[FM8.plus]\n%s\n' "$1" > "$INI"; }
run() { printf '%-34s ' "$1"; shift; "$@" 2>&1 | grep -E "RESULT|of audio|note-ons|PASS|FAIL|ERROR|VALIDATION" | tr '\n' ' '; echo; }
au() { sh tools/mac/ingui.sh "$B/auprobe $*"; }

for m in 1 2; do
    ini "arp_mode=$m"
    run "arp mode $m, VST2" "$B/vst2probe" "$P/VST/FM8.plus.vst" --arp
    run "arp mode $m, VST3" "$B/vst3probe" "$P/VST3/FM8.plus.vst3" --arp
    run "arp mode $m, AU" au --arp
done
ini "morph_cc=11"
run "morph, VST2" "$B/vst2probe" "$P/VST/FM8.plus.vst" --morph
run "morph, VST3" "$B/vst3probe" "$P/VST3/FM8.plus.vst3" --morph
run "morph, AU" au --morph
ini ""
run "AU state round trip" au --state
run "auval" sh tools/mac/ingui.sh "auval -v aumu F8pl Msca | tail -3"
ini "arp_mode=1"
run "CPU, stock FM8 VST3" "$B/vst3probe" /Library/Audio/Plug-Ins/VST3/FM8.vst3 --bench
run "CPU, FM8.plus VST3" "$B/vst3probe" "$P/VST3/FM8.plus.vst3" --bench
ini ""
run "+ menu, VST2" "$B/vst2probe" "$P/VST/FM8.plus.vst" --editor /tmp/fm8plus_vst2.png 118.4 47
run "+ menu, VST3" "$B/vst3probe" "$P/VST3/FM8.plus.vst3" --editor /tmp/fm8plus_vst3.png 118.4 47
run "+ menu, AU" au --editor /tmp/fm8plus_au.png 118.4 47
ini ""
