#!/usr/bin/env python3
"""Checks for a comment pass over bambi's C++.

    .claude/tools/comments.py same [--base REF] [files...]   the code with comments stripped is unchanged against
                                                             REF (default HEAD) -- a comment pass changed only
                                                             comments. No files: every changed C++ file.
    .claude/tools/comments.py density [files or dirs...]     comment lines as a share of non-blank lines, highest
                                                             first; default: core host editor ui plugins tools tests

`same` exits 1 when any file's code differs. Whitespace is ignored; string and character literals, raw strings and
digit separators (1'000) are read as code, so a comment marker inside them is not mistaken for a comment.
"""
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SOURCES = (".h", ".hpp", ".cpp", ".mm")
DEFAULT_DIRS = ["core", "host", "editor", "ui", "plugins", "tools", "tests"]
SKIP = ("third_party/", "doctest.h")


def strip_comments(text: str) -> str:
    """The text with every comment removed and whitespace collapsed, literals kept as they are."""
    out, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        if text.startswith("//", i):
            i = text.find("\n", i)
            i = n if i < 0 else i
        elif text.startswith("/*", i):
            end = text.find("*/", i + 2)
            i = n if end < 0 else end + 2
            out.append(" ")
        elif c == "R" and text.startswith('R"', i):  # raw string: R"delim( ... )delim"
            m = re.match(r'R"([^(\s]*)\(', text[i:])
            if m:
                close = ")" + m.group(1) + '"'
                end = text.find(close, i + m.end())
                end = n if end < 0 else end + len(close)
                out.append(text[i:end])
                i = end
            else:
                out.append(c)
                i += 1
        elif c == '"':
            j = i + 1
            while j < n and text[j] != '"':
                j += 2 if text[j] == "\\" else 1
            out.append(text[i : j + 1])
            i = j + 1
        elif c == "'":
            # a digit separator sits between two digits: 1'000
            if i > 0 and text[i - 1].isalnum() and i + 1 < n and text[i + 1].isalnum() and text[i - 1].isdigit():
                out.append(c)
                i += 1
                continue
            j = i + 1
            while j < n and text[j] != "'":
                j += 2 if text[j] == "\\" else 1
            out.append(text[i : j + 1])
            i = j + 1
        else:
            out.append(c)
            i += 1
    return re.sub(r"\s+", " ", "".join(out)).strip()


def changed_files(base: str):
    names = subprocess.run(["git", "diff", "--name-only", base, "--"], cwd=ROOT, capture_output=True, text=True,
                           check=True).stdout.split()
    return [f for f in names if f.endswith(SOURCES) and not f.startswith(SKIP)]


def at(base: str, path: str):
    r = subprocess.run(["git", "show", f"{base}:{path}"], cwd=ROOT, capture_output=True, text=True)
    return r.stdout if r.returncode == 0 else None


def same(args):
    base = "HEAD"
    if "--base" in args:
        k = args.index("--base")
        base = args[k + 1]
        args = args[:k] + args[k + 2 :]
    files = args or changed_files(base)
    bad = 0
    for f in files:
        before = at(base, f)
        now = (ROOT / f).read_text(errors="replace") if (ROOT / f).exists() else None
        if before is None or now is None:
            print(f"  new or gone  {f}")
            bad += 1
        elif strip_comments(before) != strip_comments(now):
            print(f"  CODE CHANGED {f}")
            bad += 1
        else:
            print(f"  comments only {f}")
    print(f"comments: {len(files) - bad} of {len(files)} changed only comments against {base}")
    return 1 if bad else 0


def density(args):
    targets = [ROOT / a for a in (args or DEFAULT_DIRS)]
    files = []
    for t in targets:
        files += [t] if t.is_file() else [p for p in t.rglob("*") if p.suffix in SOURCES]
    rows = []
    for p in files:
        rel = str(p.relative_to(ROOT))
        if rel.startswith(SKIP) or "/doctest.h" in rel:
            continue
        lines = [l.strip() for l in p.read_text(errors="replace").splitlines() if l.strip()]
        if len(lines) < 20:
            continue
        inside, comment = False, 0
        for l in lines:
            if inside:
                comment += 1
                inside = "*/" not in l
            elif l.startswith("//"):
                comment += 1
            elif l.startswith("/*"):
                comment += 1
                inside = "*/" not in l
        rows.append((comment / len(lines), comment, len(lines), rel))
    for share, c, total, rel in sorted(rows, reverse=True)[:40]:
        print(f"  {share:5.0%}  {c:4d} of {total:4d}  {rel}")
    return 0


if __name__ == "__main__":
    if len(sys.argv) < 2 or sys.argv[1] not in ("same", "density"):
        sys.exit(__doc__)
    sys.exit(same(sys.argv[2:]) if sys.argv[1] == "same" else density(sys.argv[2:]))
