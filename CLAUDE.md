@AGENTS.md

# For Claude in this repository

[AGENTS.md](AGENTS.md) above holds the rules, the build and where things live. This adds what is Claude's.

## The docs

[docs/VISION.md](docs/VISION.md) for what bambi is after (the yardstick for whether an idea fits), [docs/STATUS.md](docs/STATUS.md) for
what exists, [docs/DECISIONS.md](docs/DECISIONS.md) for why — an index with a status per entry; open only the
entries you need. [docs/RELEASE.md](docs/RELEASE.md) is what must be true before the first build ships,
[docs/LISTENING.md](docs/LISTENING.md) what waits on ears, an array or a host session. Pressing work is a GitHub
issue; an idea becomes one only when someone needs it.

**Read cheaply.** `grep` before `Read`; a long doc with `offset`/`limit`; never a whole doc for one fact.
**Write** with the `write-doc` skill: one concept, one page, edited in place; a decision that changes gets a new
number; a dead end is a decision with **Status:** rejected; a finished plan or build log is deleted, not archived.
Run `tools/check-docs.sh` (links, decision numbers, size budgets) after doc changes.

## Reviewers and skills

Five **reviewer** agents (`.claude/agents/`) check finished work. After a broader phase — a feature, a bug fix, an
open question resolved — run the relevant one, fix what it finds in one pass, and say which you used. A checkpoint,
not a crutch.

| reviewer | checks |
|---|---|
| `core-architecture` | layer boundaries (core / shared / one plugin), shared abstractions, the link bus, state, parameters, undo, CMake |
| `realtime-safety` | the audio thread and thread boundaries, determinism, session portability |
| `dsp-performance` | per-sample and per-block code, benchmarks, allocation and RAM |
| `ui-uniformity` | UI and UX across every plugin: colours, interactions, component reuse |
| `wording` | any text a musician or contributor reads: the audio world's terms, not bambi's own; saying what a thing does |

Several in parallel for a phase that touches several areas.

**Skills** (`.claude/skills/`) are procedures an agent carries out, loaded when the task matches:

| skill | for |
|---|---|
| `add-parameter` | adding or changing a host-automatable key, or a modulation target |
| `add-plugin-target` | a new plugin or plugin tool: CMake, identity, check suite, conformance, CI |
| `write-doc` | where a fact goes; when a decision earns an entry |
| `comment-pass` | comments and docstrings, proven to change nothing else |
| `new-prototype` | settling feel or sound in the browser first, at safe levels |
| `github-issues` | issues, labels, epics and pull requests |

Measuring cost is the `dsp-performance` reviewer's job; changing the link bus is `docs/suite/LINK.md`; testing in a
real host is a person's checklist, `docs/HOST-CHECKS.md`. A new skill earns its place only as a procedure an agent
repeats — a rule belongs in an `AGENTS.md`, a fact on a concept page.

## Contributing as Claude

Commits made with Claude end with its `Co-Authored-By: Claude` trailer, and pull request descriptions end with the
"Generated with Claude Code" line. A maintainer's own notes for Claude go in `CLAUDE.local.md`, which is not
committed.
