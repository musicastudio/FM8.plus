#!/bin/sh
# Build FM8.plus for macOS with the Xcode command line tools: sh tools/mac/build.sh [out-dir]
# Output bundles land in build-mac/ (ad-hoc signed). No CMake or Xcode project needed.
set -e
cd "$(dirname "$0")/../.."
OUT=${1:-build-mac}
mkdir -p "$OUT"
MIN=-mmacosx-version-min=10.14
VER=${FM8PLUS_VERSION:-1.1.0}   # CI passes the release tag
CXX="clang++ -std=c++17 -O2 -fobjc-arc -fvisibility=hidden -Wall -Wno-unused-function $MIN"
CORE="src/mac/core_mac.cpp src/mac/machook.cpp src/mac/settings_mac.cpp src/mac/ui_mac.mm"
FW="-framework Cocoa -framework CoreGraphics -framework ImageIO"

# bundle <dir> <exe> <id> <type> [extra plist xml]
bundle() {
    mkdir -p "$1/Contents/MacOS"
    cat > "$1/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleExecutable</key><string>$2</string>
<key>CFBundleIdentifier</key><string>$3</string>
<key>CFBundleName</key><string>FM8.plus</string>
<key>CFBundlePackageType</key><string>$4</string>
<key>CFBundleSignature</key><string>????</string>
<key>CFBundleShortVersionString</key><string>$VER</string>
<key>CFBundleVersion</key><string>$VER</string>
<key>LSMinimumSystemVersion</key><string>10.14</string>
$5
</dict></plist>
EOF
    printf '%s????' "$4" > "$1/Contents/PkgInfo"
}

# VST2: FM8.vst ships x86_64 only, so the wrapper does too.
V="$OUT/FM8.plus.vst"
bundle "$V" FM8.plus studio.musica.FM8plus.vst BNDL
$CXX -arch x86_64 -bundle -o "$V/Contents/MacOS/FM8.plus" src/mac/shim_vst2_mac.mm $CORE $FW
codesign -s - --force "$V" >/dev/null 2>&1

# AU: universal, like FM8.component. Its own type/subtype/maker so it lists beside plain FM8.
AUX='<key>AudioComponents</key><array><dict>
<key>type</key><string>aumu</string><key>subtype</key><string>F8pl</string><key>manufacturer</key><string>Msca</string>
<key>name</key><string>musica.studio: FM8.plus</string><key>description</key><string>FM8.plus</string>
<key>factoryFunction</key><string>FM8PlusAUFactory</string><key>version</key><integer>65792</integer>
<key>sandboxSafe</key><false/></dict></array>'
A="$OUT/FM8.plus.component"
bundle "$A" FM8.plus studio.musica.FM8plus.component BNDL "$AUX"
$CXX -arch x86_64 -arch arm64 -bundle -o "$A/Contents/MacOS/FM8.plus" src/mac/shim_au_mac.mm $CORE $FW     -framework AudioToolbox -framework AudioUnit -framework CoreAudioKit -framework CoreMIDI
codesign -s - --force "$A" >/dev/null 2>&1

# VST3: universal, like FM8.vst3. The Steinberg headers include themselves as "pluginterfaces/...".
mkdir -p "$OUT/inc" && ln -sfn "$PWD/third_party/vst3_pluginterfaces" "$OUT/inc/pluginterfaces"
T="$OUT/FM8.plus.vst3"
bundle "$T" FM8.plus studio.musica.FM8plus.vst3 BNDL
$CXX -arch x86_64 -arch arm64 -bundle -I"$OUT/inc" -Wno-deprecated-declarations -o "$T/Contents/MacOS/FM8.plus"     src/mac/shim_vst3_mac.mm $CORE $FW
codesign -s - --force "$T" >/dev/null 2>&1

# Test hosts.
$CXX -arch x86_64 -o "$OUT/vst2probe" tools/mac/vst2probe.mm -framework Cocoa
$CXX -arch x86_64 -o "$OUT/auprobe" tools/mac/auprobe.mm -framework Cocoa -framework AudioToolbox -framework AudioUnit -framework CoreMIDI
$CXX -arch x86_64 -I"$OUT/inc" -Wno-deprecated-declarations -o "$OUT/vst3probe" tools/mac/vst3probe.mm -framework Cocoa
echo "built into $OUT"
