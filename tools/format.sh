#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Formats bambi's own C++ sources with the repository's .clang-format. Vendored code (third_party/,
# core/tests/doctest.h) is excluded.
#
#   tools/format.sh [path...]          format in place (default: every source)
#   tools/format.sh --check [path...]  list files that are not formatted; fails if any
#
# Needs clang-format (Homebrew: brew install llvm). CLANG_FORMAT overrides which binary.

set -uo pipefail
cd "$(dirname "$0")/.."

CF=${CLANG_FORMAT:-}
if [ -z "$CF" ]; then
    for c in clang-format /opt/homebrew/opt/llvm/bin/clang-format /usr/local/opt/llvm/bin/clang-format; do
        command -v "$c" >/dev/null 2>&1 && { CF=$c; break; }
    done
fi
[ -n "$CF" ] || { echo "format: clang-format not found (brew install llvm)" >&2; exit 2; }

check=0
if [ "${1:-}" = "--check" ]; then check=1; shift; fi
[ $# -gt 0 ] || set -- core host editor ui plugins tools tests

files=$(git ls-files -- "$@" | grep -E '\.(cpp|hpp|h|mm)$' | grep -v -e '^third_party/' -e '/doctest\.h$')
[ -n "$files" ] || { echo "format: no C++ sources under: $*" >&2; exit 2; }

if [ $check -eq 0 ]; then
    echo "$files" | xargs "$CF" -i --style=file
    exit 0
fi

bad=0
while read -r f; do
    [ -n "$f" ] || continue
    "$CF" --style=file --dry-run -Werror "$f" >/dev/null 2>&1 || { echo "format: $f"; bad=1; }
done <<< "$files"
[ $bad -eq 0 ] && echo "format: every file is formatted"
exit $bad
