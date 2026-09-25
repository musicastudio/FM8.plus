#!/bin/sh
# Package the bundles from tools/mac/build.sh into the macOS release assets:
#   FM8.plus-MacOS-Installer.dmg   an installer package for all three formats, inside a disk image
#   FM8.plus-MacOS-VST2.zip, -VST3.zip, -AU.zip   the bare bundles, for people who copy them by hand
# Usage: sh tools/mac/package.sh [build-dir] [out-dir]
set -e
cd "$(dirname "$0")/../.."
B=${1:-build-mac}
OUT=${2:-dist}
VER=${FM8PLUS_VERSION:-1.0.8}
rm -rf "$OUT" && mkdir -p "$OUT"

# The zips keep each bundle's structure and signature intact.
ditto -c -k --keepParent "$B/FM8.plus.vst" "$OUT/FM8.plus-MacOS-VST2.zip"
ditto -c -k --keepParent "$B/FM8.plus.vst3" "$OUT/FM8.plus-MacOS-VST3.zip"
ditto -c -k --keepParent "$B/FM8.plus.component" "$OUT/FM8.plus-MacOS-AU.zip"

# One package, installing into the system plug-in folders beside FM8's own bundles.
W=$(mktemp -d)
ROOT="$W/root/Library/Audio/Plug-Ins"
mkdir -p "$ROOT/VST" "$ROOT/VST3" "$ROOT/Components"
ditto "$B/FM8.plus.vst" "$ROOT/VST/FM8.plus.vst"
ditto "$B/FM8.plus.vst3" "$ROOT/VST3/FM8.plus.vst3"
ditto "$B/FM8.plus.component" "$ROOT/Components/FM8.plus.component"
# Bundles are relocatable by default, so Installer would follow a stray copy elsewhere on disk;
# these always belong in /Library/Audio/Plug-Ins.
pkgbuild --analyze --root "$W/root" "$W/components.plist"
i=0
while plutil -extract "$i" xml1 -o /dev/null "$W/components.plist" >/dev/null 2>&1; do
    plutil -replace "$i.BundleIsRelocatable" -bool NO "$W/components.plist"
    i=$((i + 1))
done
mkdir -p "$W/dmg"
pkgbuild --root "$W/root" --component-plist "$W/components.plist" --install-location / \
    --identifier studio.musica.FM8plus --version "$VER" "$W/dmg/Install FM8.plus.pkg"
cat > "$W/dmg/Read Me.txt" <<EOF
FM8.plus $VER for macOS

Needs Native Instruments FM8 1.4.6 installed. "Install FM8.plus.pkg" adds FM8.plus as its own
VST2, VST3 and Audio Unit plug-in beside FM8 in /Library/Audio/Plug-Ins. FM8 itself is never
modified. Rescan plug-ins in your DAW afterwards.

To remove it, delete FM8.plus.vst, FM8.plus.vst3 and FM8.plus.component from
/Library/Audio/Plug-Ins/VST, VST3 and Components.

https://github.com/musicastudio/FM8.plus
EOF
hdiutil create -volname "FM8.plus $VER" -srcfolder "$W/dmg" -format UDZO -ov "$OUT/FM8.plus-MacOS-Installer.dmg" >/dev/null
rm -rf "$W"
ls -l "$OUT"
