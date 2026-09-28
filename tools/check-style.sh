#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Style values live in ui/include/bambi/ui/Theme.h and nowhere else. One file for the whole
# suite, not a copy per plugin.
#
# Fails if anything else in ui/, editor/ or plugins/ uses a literal colour, a literal type size or a literal
# corner radius. The point of one theme file is that changing the look never means hunting for a
# hex value; this keeps it that way.
#
#   tools/check-style.sh

set -uo pipefail
cd "$(dirname "$0")/.."

THEME=ui/include/bambi/ui/Theme.h

# a Colour built from a literal, a named JUCE colour, a font given a numeric size, a rounded
# rectangle given a numeric radius
PATTERN='Colour[[:space:]]*[({][[:space:]]*0x|Colours::|Colour::fromRGB|Colour::fromString|FontOptions[[:space:]]*\([^)]*[0-9]+(\.[0-9]*)?f|withHeight[[:space:]]*\([[:space:]]*[0-9]|RoundedRectangle[[:space:]]*\([^;]*,[[:space:]]*[0-9]+(\.[0-9]*)?f?[[:space:]]*\)'

hits=$(grep -rnE "$PATTERN" ui editor plugins --include='*.h' --include='*.cpp' | grep -v "^$THEME:")

if [ -n "$hits" ]; then
    echo "style: literal style values outside $THEME -- name them there and use the name:"
    echo "$hits"
    exit 1
fi
echo "style: every colour, type size and corner radius in ui/, editor/ and plugins/ comes from $THEME"
