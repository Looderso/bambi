#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Stale and broken links in the documentation.
#
# Docs are kept tidy by discipline, and that discipline leans on links: a link that does not exist
# yet signals something needs writing. It only works if a wrong link is caught, so this checks the
# three kinds that actually turn up:
#
#   1. a markdown link to a file that is not there
#   2. a link to a heading anchor no heading makes
#   3. a decision reference --, [0018](DECISIONS.md), DECISIONS.md#0018 -- with no such entry
#   4. a document over its size budget
#
# It reads the tree and never the network: an http link is not this script's business.
#
#   tools/check-docs.sh
set -uo pipefail
cd "$(dirname "$0")/.."

python3 - "$@" <<'PY'
import os, re, sys, urllib.parse

root = os.getcwd()
files = ["CLAUDE.md"]
for base, _, names in os.walk("docs"):
    files += [os.path.join(base, n) for n in sorted(names) if n.endswith(".md")]

text = {f: open(f, encoding="utf-8").read() for f in files}

#  Every decision number that exists, from its own file docs/decisions/NNNN-*.md.
decisions = {n[:4] for n in os.listdir("docs/decisions") if re.match(r"0\d{3}-.*\.md$", n)}

def anchors(md):
    out = set()
    for line in re.findall(r"^#{1,6} +(.*)$", md, re.M):
        a = line.strip().lower()
        a = re.sub(r"`|\*|~", "", a)
        a = re.sub(r"[^\w \-]", "", a)
        out.add(a.replace(" ", "-"))
    return out

anchor_cache = {}
problems = []

for f in files:
    here = os.path.dirname(f)
    for target in re.findall(r"\]\(([^)]+)\)", text[f]):
        target = target.strip()
        if target.startswith(("http://", "https://", "mailto:")) or not target:
            continue
        path, _, frag = target.partition("#")
        path = urllib.parse.unquote(path)
        frag = urllib.parse.unquote(frag)

        resolved = os.path.normpath(os.path.join(here, path)) if path else f
        if path and not os.path.exists(resolved):
            problems.append(f"  BROKEN     {f} -> {target}")
            continue
        #  an anchor, in a markdown file we can read
        if frag and resolved.endswith(".md") and os.path.exists(resolved):
            if resolved not in anchor_cache:
                anchor_cache[resolved] = anchors(open(resolved, encoding="utf-8").read())
            #  a decision anchor is its number, which the headings do not spell that way
            if frag not in anchor_cache[resolved] and not re.fullmatch(r"0\d{3}", frag):
                problems.append(f"  NO ANCHOR  {f} -> {target}")

    #  decision references, in every form the docs use
    seen = set()
    for m in re.finditer(r"DECISIONS\.md#(0\d{3})|\((?:DECISIONS )?(0\d{3})\)|\[(0\d{3})\]", text[f]):
        n = m.group(1) or m.group(2) or m.group(3)
        if n in seen:
            continue
        seen.add(n)
        if n not in decisions:
            problems.append(f"  NO SUCH DECISION {f} -> {n}")

#  4. a document past its size budget: a page that is read whole must stay cheap to read.
#     decisions/ is one decision per file and status/ is read by section, so they are held to the entry cap only.
WARN, FAIL, ENTRY = 25_000, 40_000, 15_000
#  Pages already past the cap when the budget arrived; each is to be split or tightened, then removed here.
LEGACY = set()
oversize = []
for f in files:
    n = len(text[f].encode("utf-8"))
    entry = f.startswith(("docs/decisions/", "docs/status/"))
    if f in LEGACY and n > FAIL:
        oversize.append(f"  LEGACY     {f} is {n // 1000} KB, past the {FAIL // 1000} KB cap: split or tighten it")
    elif n > (ENTRY if entry else FAIL):
        problems.append(f"  TOO LONG   {f} is {n // 1000} KB; split it or tighten it (cap {(ENTRY if entry else FAIL) // 1000} KB)")
    elif not entry and n > WARN:
        oversize.append(f"  long       {f} is {n // 1000} KB (warns at {WARN // 1000} KB, fails at {FAIL // 1000} KB)")

for p in problems:
    print(p)
for w in oversize:
    print(w)
if problems:
    sys.exit(1)
print(f"docs: every link resolves and every decision referenced exists "
      f"({len(files)} files, {len(decisions)} decisions)")
PY
