# 0026 · The link across plugins: definitions travel, values do not

**Status:** accepted
**Date:** 2026-09-15  **Status when written:** decided  **Scope:** suite

**Revised by **, 2026-09-17: regions are not shared as definitions — a region is a plugin's own. The region record is a drawing record, display only, and still rides the `kLinkVersion` bump with the instance kind.

**Nothing on the bus drives audio in another plugin.** Freeze and stem renders process tracks separately, so an
effect cannot rely on another instance's live values.

- **An effect's scene shows encoder sources** as faint read-only context. The encoder's window does not show
  effect instances; no cross-kind control in v1.
- **Regions can be shared as definitions:** one instance owns a region, others keep a copy in their own state and
  are republished to when it is edited (generation-counted, as paths are). Motion is computed locally from the
  same parameters and transport, so a bounce matches. If the owner goes, the copies stay.
- **An instance kind** (encoder / space / colour) and a region record join the schema: a `kLinkVersion` bump.
- **Deferred:** a region that follows a source. It only matters when the region acts on something other than that
  source, and it would need the encoder's live position — the Tier 2 caveat with capture-to-automation. Added if
  it proves to make sense.
