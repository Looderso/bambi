#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# On a CI runner: a keychain of its own holding the Developer ID certificates, searched first, so codesign and
# productbuild find them without a prompt; and the notarisation key as a file. Prints the variables package.sh
# reads, in GITHUB_ENV's format.
#
#   MACOS_CERTS_P12 (base64) MACOS_CERTS_PASSWORD NOTARY_KEY  keychain.sh >> "$GITHUB_ENV"
set -euo pipefail

dir=${RUNNER_TEMP:?run on a CI runner, in its temporary folder}
: "${MACOS_CERTS_P12:?the certificates are missing}" "${MACOS_CERTS_PASSWORD:?their password is missing}"
: "${NOTARY_KEY:?the notarisation key is missing}"
keychain="$dir/bambi-signing.keychain-db"
password=$(uuidgen)

security create-keychain -p "$password" "$keychain"
security set-keychain-settings -lut 21600 "$keychain"
security unlock-keychain -p "$password" "$keychain"
printf '%s' "$MACOS_CERTS_P12" | base64 --decode > "$dir/certs.p12"
security import "$dir/certs.p12" -k "$keychain" -P "$MACOS_CERTS_PASSWORD" -f pkcs12 \
         -T /usr/bin/codesign -T /usr/bin/productbuild -T /usr/bin/pkgbuild >/dev/null
rm -f "$dir/certs.p12"
security set-key-partition-list -S apple-tool:,apple: -s -k "$password" "$keychain" >/dev/null
# shellcheck disable=SC2046  # the existing list, one keychain per word
security list-keychains -d user -s "$keychain" $(security list-keychains -d user | tr -d '"')
security find-identity -v -p codesigning "$keychain" | grep -q "Developer ID Application" ||
    { echo "keychain: no Developer ID Application identity in the certificates" >&2; exit 1; }

printf '%s' "$NOTARY_KEY" > "$dir/notary.p8"
echo "NOTARY_KEY_PATH=$dir/notary.p8"
