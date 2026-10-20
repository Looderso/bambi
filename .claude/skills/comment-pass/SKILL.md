---
name: comment-pass
description: Review or rewrite the comments and docstrings in bambi's C++ — after a change, before a pull request, or as a pass over a module. Removes history, decision numbers, references and comments that repeat the code; keeps units, invariants, threads and present-tense reasons; proves the edit changed nothing but comments.
---

# A comment pass

The rule is [AGENTS.md](../../../AGENTS.md)'s seventh: comments say what the code does. This is how to apply it.

## What a comment is for

What the code does and what a reader cannot see from the code:

- the **unit and range** of a number (seconds, radians, normalised 0..1, dB), where the name does not say it;
- an **invariant** or precondition the code relies on and does not check;
- **which thread** a function runs on and what it must not do there (allocate, free, lock, wait);
- **ownership and lifetime** where not obvious;
- a **reason that is true now**, which a reader needs before changing the line. Present tense.

In a test, the comment names the mistake the test catches: `Catches: …`.

## What goes

- **How the code came to be**: "used to", "no longer", "now", "at first", "was", "fixed", "until", "we tried".
- **Decision numbers and references**: `0012`, `DECISIONS`, `VISION §3`, a doc, a prototype path, a ticket.
- **Names and dates**: no people, no "Claude", no agents, no dates.
- **Restating the line below**, and **commented-out code**.

When a removed story carried a real reason, keep the reason in the present tense: "was a sleep, which is 15.6 ms
on Windows" becomes "a sleep would be a whole 15.6 ms timer tick on Windows". If nothing true remains, the comment
goes.

## House forms

`/// text` before a declaration · `///< text` after a member · `/*  text … */` for a longer explanation (two
spaces after `/*`, continuation aligned) · `//  text` (two spaces) inside a function body; single-space `// ` only
after code and for `// clang-format off` / `on`. British spelling, `--` as the dash, plain declarative sentences.

## Method

1. Only the files given, only comments. **Skip any file with someone else's uncommitted work** (`git status`) and
   say so. Never `third_party/` or `core/tests/doctest.h`.
2. Read each file whole before editing it.
3. Prove it: `.claude/tools/comments.py same <files>` must report every file as "comments only".
4. `tools/format.sh <files>`, then build what you touched — a stray `*/` is a compile error.
5. **Strings are code**: a test name or log message carrying a number or history is fixed in a separate commit.
6. `.claude/tools/comments.py density <paths>` shows comment share; a header far above a third usually tells a
   story. A pointer, not a target — never delete a contract to lower it.

Report per file what kind of thing went (history, references, restating, units added), the checker's verdict,
and any comment whose reason could not be verified against the code. Do not commit unless asked.
