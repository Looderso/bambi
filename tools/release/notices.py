#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Writes THIRD-PARTY-NOTICES.txt: the copyright and licence of everything compiled into the plugins.

bambi's own dependencies are listed below. JUCE's come from its SPDX bill of materials: every library
vendored inside a JUCE module the plugins link, following the modules' dependencies, less the ones the
build never compiles. Run it after updating any dependency; `--check` fails if the file is out of date.

    tools/release/notices.py [--check]
"""
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
JUCE = ROOT / "third_party" / "JUCE"
OUT = ROOT / "THIRD-PARTY-NOTICES.txt"

#  The JUCE modules the plugins link; the rest follow from the dependencies between modules.
LINKED_MODULES = ["juce-audio-plugin-client", "juce-audio-processors", "juce-audio-utils"]

#  Vendored in a linked module, but never compiled into bambi.
NOT_COMPILED = {
    "Oboe": "Android only",
    "ASIO SDK": "JUCE_ASIO is off",
    "AAX SDK": "bambi builds no AAX",
    "AudioUnitSDK": "bambi builds no Audio Unit",
    "LV2": "LV2 hosting is off",
    "lilv": "LV2 hosting is off",
    "serd": "LV2 hosting is off",
    "sord": "LV2 hosting is off",
    "sratom": "LV2 hosting is off",
    "@juce-framework/webview": "part of JUCE itself; web views are off",
}

#  bambi's own dependencies that reach the plugins: name, where, what, licence file.
OWN = [
    ("clap-juce-extensions", "third_party/clap-juce-extensions", "the CLAP format for JUCE (patched)", "LICENSE.md"),
    ("CLAP", "third_party/clap-juce-extensions/clap-libs/clap", "the CLAP plugin API", "LICENSE"),
    ("clap-helpers", "third_party/clap-juce-extensions/clap-libs/clap-helpers", "CLAP helper classes", "LICENSE"),
    ("pffft", "third_party/pffft", "the FFT for the spectral features", "LICENSE"),
    ("yyjson", "third_party/yyjson", "reading and writing state and presets", "LICENSE"),
    ("dr_libs", "third_party/dr_libs", "reading and writing WAV files", "LICENSE"),
]

LICENCE_FILE = re.compile(r"^(licen[cs]e|copying)|licen[cs]e\.txt$", re.I)
RULE = "=" * 100


def licence_text(folder: Path, name: str) -> str:
    """The licence as the component states it."""
    if name == "jpeglib":  # the terms are a section of its README
        readme = (folder / "README").read_text(errors="replace")
        return readme[readme.index("\nLEGAL ISSUES\n") + 1 : readme.index("\nREFERENCES\n")].strip()
    if name == "pslextensions":
        return "Public domain."
    files = sorted(f for f in folder.iterdir() if f.is_file() and LICENCE_FILE.search(f.name))
    if not files:
        sys.exit(f"notices: no licence file for {name} in {folder}")
    return "\n\n".join(f.read_text(errors="replace").strip() for f in files)


def juce_components():
    bom = json.loads((JUCE / "JUCE.spdx.json").read_text())
    depends, contains = {}, {}
    for r in bom["relationships"]:
        a, kind, b = r["spdxElementId"], r["relationshipType"], r["relatedSpdxElement"]
        if kind == "DEPENDS_ON":
            depends.setdefault(a, []).append(b)
        elif kind == "CONTAINS" and a != "SPDXRef-JUCE":
            contains.setdefault(a, []).append(b)
    modules, todo = set(), ["SPDXRef-" + m for m in LINKED_MODULES]
    while todo:
        m = todo.pop()
        if m not in modules:
            modules.add(m)
            todo += depends.get(m, [])
    packages = {p["SPDXID"]: p for p in bom["packages"]}
    found = []
    for module in sorted(modules):
        for ref in contains.get(module, []):
            p = packages[ref]
            if p["name"] in NOT_COMPILED:
                continue
            where = re.search(r"Vendored at (\S+?)[;,.]?(\s|$)", p.get("sourceInfo", "")).group(1)
            version = p.get("versionInfo", "")
            found.append((p["name"], "" if version == "NOASSERTION" else version, p["licenseConcluded"], JUCE / where))
    return sorted(found, key=lambda c: c[0].lower())


def notices() -> str:
    out = [
        "bambi — third-party notices",
        "",
        "Copyright (C) 2026 Lorenz Häusler and the bambi contributors.",
        "",
        "bambi's own code is licensed under the GNU General Public License, version 3 or later (LICENSE).",
        "The plugins include JUCE, which bambi uses under the GNU Affero General Public License, version 3,",
        "so the plugins as distributed are licensed under the AGPLv3 (LICENSE-AGPL-3.0.md). The complete",
        "source of each release, JUCE included, is attached to that release:",
        "https://github.com/Looderso/bambi/releases",
        "",
        "VST is a registered trademark of Steinberg Media Technologies GmbH.",
        "",
        "The plugins contain the following third-party components, under the licences reproduced below.",
        "",
    ]
    own = [(n, "", what, ROOT / where / f) for n, where, what, f in OWN]
    juce = juce_components()
    out.append("  JUCE 9.0.2 — AGPL-3.0 (LICENSE-AGPL-3.0.md), Copyright (c) Raw Material Software Limited")
    for n, _, what, _ in own:
        out.append(f"  {n} — {what}")
    for n, version, lic, _ in juce:
        out.append(f"  {n} {version}".rstrip() + f" — {lic}, bundled with JUCE")
    for n, _, _, file in own:
        out += ["", RULE, n, RULE, "", file.read_text(errors="replace").strip()]
    for n, version, lic, folder in juce:
        out += ["", RULE, f"{n} {version}".rstrip() + f" ({lic}), bundled with JUCE", RULE, "", licence_text(folder, n)]
    return "\n".join(out) + "\n"


if __name__ == "__main__":
    text = notices()
    if "--check" in sys.argv[1:]:
        if not OUT.exists() or OUT.read_text() != text:
            sys.exit("notices: THIRD-PARTY-NOTICES.txt is out of date; run tools/release/notices.py")
        print("notices: up to date")
    else:
        OUT.write_text(text)
        print(f"notices: wrote {OUT.relative_to(ROOT)}, {len(juce_components()) + len(OWN) + 1} components")
