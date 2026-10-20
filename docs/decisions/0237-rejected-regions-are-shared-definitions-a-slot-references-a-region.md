# 0237 · Rejected: “Regions are shared definitions: a slot references a region in a session-wide library.”

**Status:** rejected

*Found false by 2026-09-17.* A dead end, recorded so it is not walked twice.

Written in
[REGIONS §6](../suite/REGIONS.md) and [ECHO §6](../echo/ECHO-CONTROLS.md), then narrowed on 2026-09-17 to a second
kind, the *session region*, and withdrawn the same day. What was wrong: a shared
region that is automatable has conflicting lanes, or makes another instance's audio depend on the
owner's live value ([0026](0026-the-link-across-plugins-definitions-travel.md)); made safe — no host parameters, transport-driven motion
only — it was a library, an ownership question and a conversion question for a case the user judges
rarer than suspected. **The follower variant fails differently and is worth remembering:** a region
that follows another plugin's own region would need the master's *movement*, which comes from host
automation and the matrix and exists elsewhere only as a live value — up to ~33 ms late through the
message thread, wrong in an offline bounce where the link ticks on the wall clock, and still when the
follower's track is frozen without the master. It is the same problem as
another instance as a modulation source and returns if that is solved.
What covers the shared case: the identical parameter layout, so a lane copies between tracks; a
one-shot *copy region from ▸ instance*; other instances' regions drawn, display only.
