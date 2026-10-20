---
name: write-doc
description: Write or change bambi's documentation — record a decision or a dead end, update a concept page after a change, decide whether something needs its own page, or find where a fact belongs. Use whenever something is decided, discovered, found false or finished.
---

# Writing a doc

## Where a fact goes

| what | where |
|---|---|
| how something is, now | its concept page, edited in place — present tense, no history, no decision numbers in the text |
| why it is that way | a decision, `docs/decisions/NNNN-slug.md`, plus one line in `docs/DECISIONS.md` |
| a route believed and shown false | a decision with **Status:** rejected |
| how to do a recurring task | a skill in `.claude/skills/` |
| something pressing (a bug, debt that blocks, a release need) | a GitHub issue; ship blockers also in `docs/RELEASE.md` |
| an idea or nice-to-have nobody has asked for | not an issue — tell the maintainer; it becomes an issue when someone needs it |
| something that waits on ears, an array or a host session | `docs/LISTENING.md` |
| a plan, a handoff, a build log | nowhere permanent — delete it when the work is done; git keeps it |

**A new page** only for a concept referred to from more than one place that cannot be understood from the code
(a new engine, interaction model or shared mechanism). A parameter, a fix or a rename updates an existing page. A
new page gets a row in `docs/README.md`. A document lives in the narrowest folder that covers everything it says;
anything two plugins share goes in `docs/suite/`. File names are unique across folders.

## A decision

**Write one only when** the reason is not visible in the code and someone would otherwise undo it, when it records a
dead end someone would plausibly try again, or when it is a constraint from outside the code (a licence, a format, a
release rule). How a control looks, how a bug was fixed, what a phase built, what a page or a test already says —
none of that is a decision. When in doubt, a line on the concept page is enough.

```markdown
# NNNN · <the decision as a sentence>

**Status:** accepted

*YYYY-MM-DD. <what prompted it, one line>*

<what was decided; why, in a few lines; what it is contingent on; what would overturn it>
```

- The next free number; the index line is `| NNNN | accepted | <title> |`.
- Status is one of: `accepted`, `accepted, revised in part by [N](…)`, `superseded by [N](…)`, `deprecated — <why>`,
  `rejected`. **A decision that changes gets a new number**; the old entry's Status line and index row name it.
  The old text is never rewritten.
- **A rejected decision** says what was believed, what was actually wrong (with the measurement), and what is done
  instead. Title: `Rejected: “<the claim>”`.
- Short: evidence and long argument link to the concept page. Under 15 KB.
- Published text names no one — no people, no employer, no "the user said" quotes — and no local paths.

## Rules that hold everywhere

- **One concept, one page, edited in place.** Rewrite the paragraph that is now wrong; never append.
- **Measured, not asserted.** A number says which run and which build (Release, % of a core at 128 / 48 kHz).
- **Read cheaply.** `grep -n '^#'` for the outline, then read with offset/limit. A page read whole stays under
  25 KB (fails at 40 KB).
- **Code comments** say what the code does — never a decision number, a doc reference or how it came to be.
- Plain voice, tables for anything comparative, no marketing, no hedging.

## Finish

Run `tools/check-docs.sh` (broken links, missing decisions, size budgets).
