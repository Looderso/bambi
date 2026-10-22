#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The macOS release: every bundle signed with a Developer ID from the inside out, under the hardened runtime, then
# an installer package with two choices -- the plugins, and the live host with its virtual audio driver -- and a zip of the
# plugins for copying by hand. Both notarised with one submission, the package's, whose tickets are then stapled.
#
#   package.sh <out> <plugins> <host.app> <bambi.driver> <version>
#
# <plugins> holds VST3/, CLAP/, and the notes and licences that go into the zip beside them; the licences are also
# installed, with the uninstaller, in /Library/Application Support/bambi. Writes
# <out>/bambi-macos.pkg and <out>/bambi-macos.zip.
#
# Identities, by the start of their names in the keychain: BAMBI_SIGN_APP (default "Developer ID Application") and
# BAMBI_SIGN_INSTALLER (default "Developer ID Installer"). Notarised with NOTARY_PROFILE, a notarytool keychain
# profile, or NOTARY_KEY_PATH, NOTARY_KEY_ID and NOTARY_ISSUER_ID, an App Store Connect API key; with neither,
# everything is signed and packaged but not notarised, which only this Mac accepts.
set -euo pipefail

[ $# -eq 5 ] || { sed -n '8,9p' "$0" >&2; exit 2; }
out=$1 plugins=$2 host=$3 driver=$4 version=$5
here=$(cd "$(dirname "$0")" && pwd)
app_id=${BAMBI_SIGN_APP:-Developer ID Application}
installer_id=${BAMBI_SIGN_INSTALLER:-Developer ID Installer}

mkdir -p "$out"
out=$(cd "$out" && pwd)
stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT

#  ---- signing, the innermost code first: a bundle signed before what it contains is invalid --------------------

sign() {  # <bundle> [codesign options]
    local bundle=$1; shift
    codesign --force --options runtime --timestamp --sign "$app_id" "$@" "$bundle"
}

mkdir -p "$stage/zip" "$stage/host" "$stage/driver"
cp -R "$plugins"/. "$stage/zip/"
cp -R "$host" "$stage/host/"
cp -R "$driver" "$stage/driver/"
host_app="$stage/host/$(basename "$host")"
driver_bundle="$stage/driver/$(basename "$driver")"

shopt -s nullglob
bundles=("$stage"/zip/VST3/*.vst3 "$stage"/zip/CLAP/*.clap)
[ ${#bundles[@]} -gt 0 ] || { echo "package: no plugin bundles in $plugins" >&2; exit 1; }
for bundle in "${bundles[@]}"; do
    for helper in "$bundle"/Contents/Helpers/*.app; do sign "$helper"; done
    sign "$bundle"
done
sign "$host_app" --entitlements "$here/live-host.entitlements"
sign "$driver_bundle"

#  Every executable runs on the oldest macOS the release supports; a build that took its machine's would not.
oldest=14.0
for code in "${bundles[@]}" "$host_app" "$driver_bundle"; do
    codesign --verify --strict --deep "$code" || { echo "package: $code does not verify" >&2; exit 1; }
    for binary in "$code"/Contents/MacOS/* "$code"/Contents/Helpers/*.app/Contents/MacOS/*; do
        while read -r minos; do
            if [ "$(printf '%s\n%s\n' "$minos" "$oldest" | sort -V | tail -1)" != "$oldest" ]; then
                echo "package: $binary needs macOS $minos, later than $oldest" >&2
                exit 1
            fi
        done < <(vtool -show-build "$binary" | awk '$1 == "minos" { print $2 }')
    done
done

#  ---- the component packages ----------------------------------------------------------------------------------

#  A component package moves a bundle to wherever one with its id already is, a build folder included, unless told
#  not to: every bundle goes where the package says.
fixed_place() {  # <root> <component plist to write>
    pkgbuild --analyze --root "$1" "$2" >/dev/null
    local i=0
    while plutil -extract "$i" xml1 -o /dev/null "$2" >/dev/null 2>&1; do
        plutil -replace "$i.BundleIsRelocatable" -bool NO "$2"
        i=$((i + 1))
    done
}

#  Each preinstall removes the bundles its package brings, so none is updated file by file in place: macOS keeps a
#  replaced binary's old signature, and Core Audio refuses a driver whose code no longer matches it.
preinstall() {  # <scripts folder> <path under the install location>...
    local script=$1/preinstall; shift
    mkdir -p "$(dirname "$script")"
    {
        echo '#!/bin/sh'
        echo '# $2: the install location'
        echo 'cd "$2" || exit 0'
        for p in "$@"; do printf 'rm -rf "%s"\n' "$p"; done
    } > "$script"
    chmod 755 "$script"
}

component() {  # <name> <root> <install location> <scripts folder or "">
    local name=$1 root=$2 location=$3 scripts=$4
    fixed_place "$root" "$stage/$name.plist"
    pkgbuild --root "$root" --component-plist "$stage/$name.plist" --install-location "$location" \
             ${scripts:+--scripts "$scripts"} --identifier "org.bambi.pkg.$name" --version "$version" \
             "$stage/pkgs/$name.pkg" >/dev/null
}

mkdir -p "$stage/pkgs" "$stage/roots/plugins/VST3" "$stage/roots/plugins/CLAP" "$stage/roots/support"
cp -R "$stage"/zip/VST3/*.vst3 "$stage/roots/plugins/VST3/"
cp -R "$stage"/zip/CLAP/*.clap "$stage/roots/plugins/CLAP/"
names=()
for bundle in "${bundles[@]}"; do names+=("$(basename "$(dirname "$bundle")")/$(basename "$bundle")"); done
preinstall "$stage/scripts/plugins" "${names[@]}"
component plugins "$stage/roots/plugins" "/Library/Audio/Plug-Ins" "$stage/scripts/plugins"

preinstall "$stage/scripts/livehost" "$(basename "$host_app")"
component livehost "$stage/host" "/Applications" "$stage/scripts/livehost"

preinstall "$stage/scripts/driver" "$(basename "$driver_bundle")"
printf '#!/bin/sh\nkillall coreaudiod 2>/dev/null\nexit 0\n' > "$stage/scripts/driver/postinstall"
chmod 755 "$stage/scripts/driver/postinstall"
component driver "$stage/driver" "/Library/Audio/Plug-Ins/HAL" "$stage/scripts/driver"

cp "$here/uninstall.sh" "$stage/roots/support/uninstall.sh"
for notice in LICENSE LICENSE-AGPL-3.0.md THIRD-PARTY-NOTICES.txt; do
    [ -f "$plugins/$notice" ] && cp "$plugins/$notice" "$stage/roots/support/"
done
chmod 755 "$stage/roots/support/uninstall.sh"
pkgbuild --root "$stage/roots/support" --install-location "/Library/Application Support/bambi" \
         --identifier org.bambi.pkg.support --version "$version" "$stage/pkgs/support.pkg" >/dev/null

#  ---- the installer: two choices, the uninstaller with either ---------------------------------------------------

cat > "$stage/distribution.xml" <<EOF
<?xml version="1.0" encoding="utf-8"?>
<installer-gui-script minSpecVersion="2">
    <title>bambi $version</title>
    <options customize="always" require-scripts="false" hostArchitectures="arm64,x86_64"/>
    <domains enable_localSystem="true"/>
    <choices-outline>
        <line choice="plugins"/>
        <line choice="livehost"/>
    </choices-outline>
    <choice id="plugins" title="Plugins"
            description="The ambisonics plugins as VST3 and CLAP, for every user of this Mac.">
        <pkg-ref id="org.bambi.pkg.plugins"/>
        <pkg-ref id="org.bambi.pkg.support"/>
    </choice>
    <choice id="livehost" title="Live host"
            description="An app that plays your Ableton Live set through the bambi plugins, and a virtual audio driver that sends Live's tracks to it. Audio on this Mac stops for a moment at the end of the installation.">
        <pkg-ref id="org.bambi.pkg.livehost"/>
        <pkg-ref id="org.bambi.pkg.driver"/>
        <pkg-ref id="org.bambi.pkg.support"/>
    </choice>
    <pkg-ref id="org.bambi.pkg.plugins" version="$version" onConclusion="none">plugins.pkg</pkg-ref>
    <pkg-ref id="org.bambi.pkg.livehost" version="$version" onConclusion="none">livehost.pkg</pkg-ref>
    <pkg-ref id="org.bambi.pkg.driver" version="$version" onConclusion="none">driver.pkg</pkg-ref>
    <pkg-ref id="org.bambi.pkg.support" version="$version" onConclusion="none">support.pkg</pkg-ref>
</installer-gui-script>
EOF
pkg="$out/bambi-macos.pkg"
productbuild --distribution "$stage/distribution.xml" --package-path "$stage/pkgs" --sign "$installer_id" \
             --timestamp "$pkg" >/dev/null
pkgutil --check-signature "$pkg" >/dev/null || { echo "package: $pkg is not signed" >&2; exit 1; }

#  ---- notarised once, through the package: its ticket covers every bundle inside --------------------------------

if [ -n "${NOTARY_PROFILE:-}" ]; then
    auth=(--keychain-profile "$NOTARY_PROFILE")
elif [ -n "${NOTARY_KEY_PATH:-}" ]; then
    auth=(--key "$NOTARY_KEY_PATH" --key-id "$NOTARY_KEY_ID" --issuer "$NOTARY_ISSUER_ID")
else
    auth=()
fi

if [ ${#auth[@]} -gt 0 ]; then
    #  A rejected submission may end notarytool with an error; its result still names the submission.
    result=$(xcrun notarytool submit "$pkg" "${auth[@]}" --wait --timeout 2h --output-format json) || true
    status=$(plutil -extract status raw -o - - <<< "$result" 2>/dev/null || true)
    if [ "$status" != Accepted ]; then
        echo "package: notarisation says ${status:-nothing}: $result" >&2
        id=$(plutil -extract id raw -o - - <<< "$result" 2>/dev/null || true)
        [ -n "$id" ] && xcrun notarytool log "$id" "${auth[@]}" >&2
        exit 1
    fi
    #  A ticket can take a while to be served after the submission is accepted.
    staple() {
        local try
        for try in 1 2 3 4 5; do
            xcrun stapler staple -q "$1" && return 0
            sleep $((try * 10))
        done
        echo "package: no ticket to staple to $1" >&2
        return 1
    }
    staple "$pkg"
    for bundle in "${bundles[@]}"; do staple "$bundle"; done
else
    echo "package: no notarisation credentials; signed but not notarised" >&2
fi

ditto -c -k "$stage/zip" "$out/bambi-macos.zip"
echo "package: wrote $pkg and $out/bambi-macos.zip"
