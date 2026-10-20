#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The loop device: BlackHole (GPL-3.0, Existential Audio), fetched at a pinned commit, with bambi's patch --
# channel names, set from outside through a custom device property -- and built as "bambi", with its own
# bundle id so it sits beside an installed BlackHole without colliding. Signed ad hoc, which a driver built
# on this machine needs and no other machine accepts.
#
#   driver/build.sh            builds driver/build/bambi.driver
#   driver/build.sh install    and prints the two lines that install it (they need an administrator)
set -euo pipefail
cd "$(dirname "$0")"

REPO=https://github.com/ExistentialAudio/BlackHole
PIN=561d934
CHANNELS=${CHANNELS:-64}  # a test build with fewer: CHANNELS=16 driver/build.sh
FACTORY=a4a6767e-e526-4017-ac46-d7d2b53dbfa1  # bambi's own: a stock BlackHole built from source has the source's

if [ ! -d BlackHole ]; then
    git clone -q "$REPO" BlackHole
fi
git -C BlackHole checkout -q "$PIN" 2>/dev/null || { git -C BlackHole fetch -q --unshallow 2>/dev/null || true; git -C BlackHole checkout -q "$PIN"; }
if git -C BlackHole apply --check ../bambi-names.patch 2>/dev/null; then
    git -C BlackHole apply ../bambi-names.patch
elif ! git -C BlackHole apply --reverse --check ../bambi-names.patch 2>/dev/null; then
    echo "driver: bambi-names.patch neither applies nor is applied" >&2
    exit 1
fi

xcodebuild -quiet \
    -project BlackHole/BlackHole.xcodeproj \
    -configuration Release \
    CONFIGURATION_BUILD_DIR="$PWD/build" \
    PRODUCT_BUNDLE_IDENTIFIER=org.bambi.loop \
    PRODUCT_NAME=bambi \
    CODE_SIGN_IDENTITY=- CODE_SIGN_STYLE=Manual DEVELOPMENT_TEAM= \
    GCC_PREPROCESSOR_DEFINITIONS='$GCC_PREPROCESSOR_DEFINITIONS
      kDriver_Name=\"bambi\"
      kPlugIn_BundleID=\"org.bambi.loop\"
      kHas_Driver_Name_Format=false
      kDevice_Name=\"bambi\"
      kDevice2_IsHidden=true
      kNumber_Of_Channels='"$CHANNELS"'
      kCanBeDefaultDevice=false
      kCanBeDefaultSystemDevice=false'

#  the suite's mark as the bundle's icon, in place of BlackHole's: rendered from the one SVG, at every size an
#  .icns holds
ICONSET=build/bambi.iconset
rm -rf "$ICONSET" && mkdir -p "$ICONSET"
sips -s format png ../ui/assets/marks/bambi.svg --out "$ICONSET/icon_512x512@2x.png" >/dev/null
for size in 16 32 128 256 512; do
    sips -z $size $size "$ICONSET/icon_512x512@2x.png" --out "$ICONSET/icon_${size}x${size}.png" >/dev/null
    [ $size = 512 ] || sips -z $((size * 2)) $((size * 2)) "$ICONSET/icon_512x512@2x.png" \
        --out "$ICONSET/icon_${size}x${size}@2x.png" >/dev/null
done
iconutil -c icns "$ICONSET" -o build/bambi.driver/Contents/Resources/BlackHole.icns
rm -rf "$ICONSET"

#  the plug-in factory's id is the bundle's to declare; the source's default is every self-built BlackHole's
PLIST=build/bambi.driver/Contents/Info.plist
OLD=$(/usr/libexec/PlistBuddy -c "Print :CFPlugInTypes:443ABAB8-E7B3-491A-B985-BEB9187030DB:0" "$PLIST")
if [ "$OLD" != "$FACTORY" ]; then
    /usr/libexec/PlistBuddy -c "Delete :CFPlugInFactories:$OLD" \
        -c "Add :CFPlugInFactories:$FACTORY string BlackHole_Create" \
        -c "Set :CFPlugInTypes:443ABAB8-E7B3-491A-B985-BEB9187030DB:0 $FACTORY" "$PLIST"
fi
codesign --force --sign - build/bambi.driver  # once more: the icon and the plist changed after Xcode signed

echo "built $PWD/build/bambi.driver ($CHANNELS channels)"
if [ "${1:-}" = install ]; then
    echo "to install, as an administrator -- the old bundle removed first: a signed binary copied over another"
    echo "in place keeps the old one's signature, and Core Audio's driver helper is killed loading it"
    echo "  sudo rm -rf /Library/Audio/Plug-Ins/HAL/bambi.driver"
    echo "  sudo cp -R \"$PWD/build/bambi.driver\" /Library/Audio/Plug-Ins/HAL/"
    echo "  sudo killall coreaudiod"
fi
