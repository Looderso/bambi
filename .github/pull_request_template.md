## What this changes

<!-- One or two sentences: how things are after this change, and why. Link the issue it answers. -->

## Checked

- [ ] `tools/verify.sh` passes
- [ ] **Goldens:** unchanged, or these moved and this is why: <!-- which, and why -->
- [ ] New logic has tests, each with a `Catches:` line naming the mistake it catches
- [ ] **The window** (if it changed): a picture from the plugin's picture tool is attached
- [ ] **In a host** (if it can be heard or seen): tried after rebuilding and restarting the host, in <!-- host, format, system -->

## Before merging

- [ ] Form and behaviour are in separate commits, each subject a sentence that says how things are now
- [ ] Comments say what the code does, with no history, names or references
- [ ] A design change or a changed parameter key was agreed in an issue first
- [ ] If the link bus's shared-memory layout changed, `kLinkVersion` is bumped

<!-- The rules behind this list: https://bambi.wiki/contribute -->
