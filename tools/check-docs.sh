#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Stale text in what agents and contributors read: the docs, every AGENTS.md, the skills and reviewers, README and
# CONTRIBUTING. Text goes stale when the code moves under it, so this checks what a move breaks:
#
#   1. a markdown link to a file that is not there, or to an anchor no heading makes -- lychee checks these
#      (lychee.toml; brew install lychee), offline
#   2. a decision reference -- (0018), [0018], DECISIONS.md#0018 -- with no such entry
#   3. a repository path in `backticks` that names nothing in the tree (a renamed file, a moved header)
#   4. a file past its size budget: everything here is read whole, so it must stay cheap to read
#
# Decisions are exempt from 3: each records what was true when it was written. Paths under prototypes/ are not
# published and are not checked. It reads the tree and never the network.
#
#   tools/check-docs.sh                links, then bambi's own rules
#   tools/check-docs.sh --skip-links   bambi's own rules only (CI runs lychee as its own step)
set -uo pipefail
cd "$(dirname "$0")/.." || exit 1

links=0
if [ "${1:-}" != "--skip-links" ]; then
    if ! command -v lychee >/dev/null; then
        echo "docs: lychee is not installed (brew install lychee, or cargo install lychee)"; exit 1
    fi
    lychee --config lychee.toml --no-progress 'docs/**/*.md' '*.md' '**/AGENTS.md' '.claude/**/*.md' || links=1
fi

python3 - <<'PY' || exit 1
import os, re, sys

SKIP_DIRS = {"third_party", ".git", "prototypes", "node_modules"}
def walk_md(top):
    for base, dirs, names in os.walk(top):
        dirs[:] = sorted(d for d in dirs if d not in SKIP_DIRS and not d.startswith("build") and d != "worktrees")
        for n in sorted(names):
            if n.endswith(".md"):
                yield os.path.normpath(os.path.join(base, n))

files = [f for f in ["CLAUDE.md", "README.md", "CONTRIBUTING.md"] if os.path.exists(f)]
files += list(walk_md("docs"))
files += [f for f in walk_md(".") if os.path.basename(f) == "AGENTS.md"]
files += [f for f in walk_md(".claude") if f.startswith((".claude/agents/", ".claude/skills/"))]
files = list(dict.fromkeys(files))
text = {f: open(f, encoding="utf-8").read() for f in files}

decisions = {n[:4] for n in os.listdir("docs/decisions") if re.match(r"0\d{3}-.*\.md$", n)}

#  A code span is checked as a path when it starts in one of the repository's own top-level folders and holds
#  nothing a path cannot (a space, a placeholder, a glob).
TOP = {d for d in os.listdir(".") if os.path.isdir(d) and not d.startswith(".")} | {".claude", ".github"}
TOP -= {d for d in TOP if d.startswith("build")} | {"prototypes"}
def path_candidate(span):
    s = span.strip().rstrip(".,;:")
    if not s or re.search(r"[\s<>*{}|$…~`'\"=()\[\]]", s) or s.startswith(("-", "http", "/")) or "NNNN" in s:
        return None
    if s.startswith("third_party/"):             # vendored, and a submodule may not be checked out
        return None
    s = re.sub(r":\d+(-\d+)?$", "", s)          # file.cpp:42
    if s.split("/", 1)[0] in TOP and "/" in s:
        return s
    return None

#  `ui/ChannelStrip` or `core/patch/presets` name a module by its folder: the header under include/bambi/, or its
#  source. A path git ignores (a local setting) is not the tree's to check.
def exists(p, here):
    if os.path.exists(p) or os.path.exists(os.path.join(here, p)):
        return True
    top, _, rest = p.partition("/")
    stem = os.path.splitext(rest)[0]
    lib = top if top != "core" else ""
    for cand in (f"{top}/include/bambi/{lib + '/' if lib else ''}{stem}", f"{top}/src/{stem}",
                 f"{top}/include/bambi/{stem}"):
        if any(os.path.exists(cand + ext) for ext in ("", ".h", ".hpp", ".cpp")):
            return True
    return os.system(f"git check-ignore -q {p!r} 2>/dev/null") == 0

problems, warnings = [], []

for f in files:
    here = os.path.dirname(f)
    body = text[f]
    seen = set()
    for m in re.finditer(r"DECISIONS\.md#(0\d{3})|\((?:DECISIONS )?(0\d{3})\)|\[(0\d{3})\]", body):
        n = m.group(1) or m.group(2) or m.group(3)
        if n not in seen:
            seen.add(n)
            if n not in decisions:
                problems.append(f"  NO SUCH DECISION {f} -> {n}")

    if f.startswith("docs/decisions/"):
        continue
    for span in sorted(set(re.findall(r"`([^`\n]+)`", re.sub(r"```.*?```", "", body, flags=re.S)))):
        p = path_candidate(span)
        if p and not exists(p, here):
            problems.append(f"  NO PATH    {f} -> `{p}`")

#  Budgets in bytes, (warn, fail): a decision, the root AGENTS.md, a module's, a skill or reviewer, any other page.
def budget(f):
    if f.startswith("docs/decisions/"):
        return (15_000, 15_000)
    if f == "AGENTS.md":
        return (7_000, 9_000)
    if f.endswith("AGENTS.md"):
        return (3_000, 4_500)
    if f.startswith(".claude/"):
        return (5_000, 7_000)
    return (16_000, 25_000)

for f in files:
    n = len(text[f].encode("utf-8"))
    warn, fail = budget(f)
    if n > fail:
        problems.append(f"  TOO LONG   {f} is {n // 1000} KB; split it or tighten it (cap {fail // 1000} KB)")
    elif n > warn:
        warnings.append(f"  long       {f} is {n // 1000} KB (warns at {warn // 1000} KB, fails at {fail // 1000} KB)")

for p in problems:
    print(p)
for w in warnings:
    print(w)
if problems:
    sys.exit(1)
print(f"docs: every decision and path resolves ({len(files)} files, {len(decisions)} decisions)")
PY
exit $links
