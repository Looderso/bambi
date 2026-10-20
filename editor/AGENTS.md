# editor/ — the shared editor and its check kits

The page, frame, panels and checks every plugin's window shares (`EffectEditor`, `EffectFrame`, `PatchControls`,
the `*Checks.h` kits a plugin's check suite runs). The look's rules are in `ui/AGENTS.md`.

- A plugin's editor is a configuration of these, not a copy: a difference grows an option here.
- The check kits (`EffectChecks.h`, `PresetChecks.h`, `RegionChecks.h`, `AllocationCheck.h`) are shared so every
  plugin answers the same questions; a new plugin runs them, it does not write its own.
- `AllocationCheck.h` checks that a second of `processBlock` allocates and frees nothing on the rendering thread
  (`operator new` everywhere, `malloc` too on Linux). Every plugin's check suite runs it.
