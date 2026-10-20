---
name: github-issues
description: Work on bambi's public GitHub with the gh CLI — draft or file an issue, label it, link epics and sub-issues, write a pull request's description, triage what is open. Use for anything that publishes text to github.com/Looderso/bambi.
---

# GitHub issues and pull requests

Everything written there is published. Read what is there first: `gh label list`,
`gh issue list --state open --limit 100 --json number,title,labels`. If the repository disagrees with this page,
the repository is the truth — say so.

## Labels

**Area** — every area an issue touches, not just the main one:

| label | for |
|---|---|
| `encoder`, `echo`, `reverb`, `decoder` | one plugin's behaviour, engine or window |
| `core` | `core/`: DSP, the model, anything with no JUCE |
| `ui` | the shared UI (`ui/`, `editor/`) or a plugin's window |
| `link` | the link bus between instances |
| `live` | the Ableton Live helper: `apps/live-host`, `driver/`, `platform/`, the Max for Live device |
| `ai-workflow` | coding with AI: agents, skills, how contributors use them |

**Kind** — GitHub's defaults (`bug`, `enhancement`, `documentation`, `question`, `good first issue`,
`help wanted`). **`epic`** — a parent whose children are sub-issues.

## Epics

An epic is a **goal that ends**: when its sub-issues close, it closes. A plugin or a theme never ends, so it is a
label. Link a sub-issue natively: `gh api repos/Looderso/bambi/issues/{parent}/sub_issues -F sub_issue_id=<id>`,
the child's **id** from `gh api repos/Looderso/bambi/issues/{n} --jq .id`.

## What needs the owner's yes

- **Apply** existing labels, link to an existing epic, fix a plainly wrong label: do it and list it.
- **File or edit an issue's text** only when the owner approved that text.
- **A new label or epic, closing an issue, renaming or deleting a label: never alone.** Propose it — name, colour,
  description, which issues, why the scheme does not cover it. A label earns its place when several issues share an
  area no label names.

## How an issue reads

A title that says the outcome in plain words; a body with what and why, the scope as short lists, the rules it must
keep (link the guideline at bambi.wiki/contribute instead of restating it), and how it is tested — the mutation a
test catches. An open design question says "decide in a prototype". Don't file what the site already covers.

## Pull requests

One issue, one pull request into `main`, each from its own branch named for the issue — never stacked. Merged with
a merge commit, never squash or rebase; the branch stays. The description says what changed and why, `Closes #n`,
and how it was verified (`tools/verify.sh`).

## Published text

- Names no one. Linking `docs/` is fine; code, commit messages and issue titles carry no decision numbers.
- **Work made with Claude says so**: commits end with its `Co-Authored-By: Claude …` trailer, pull request
  descriptions end with `🤖 Generated with [Claude Code](https://claude.com/claude-code)`.
- The project's conventions: degrees at the UI, AmbiX, right-handed coordinates.

Write a body to a scratch file and pass `--body-file`; a heredoc inside `--body` breaks on backticks.

## Report

What changed (issue, label, link), what is proposed and why, and anything in the repository that disagrees with
this page.
