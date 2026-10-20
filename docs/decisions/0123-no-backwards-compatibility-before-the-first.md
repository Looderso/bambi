# 0123 · No backwards compatibility before the first release

**Status:** accepted

*2026-09-21. Decided by the user: "i don't want to support backwards compatibility when we haven't
even created a public repo or created a release."*

**The state loader translates nothing.** Four migrations had accumulated in it, each written in
good faith and each for an audience of one -- the author's own test sessions: v1 envelopes kept on
audio triggers, v2 LFO divisions renumbered, a v3 region's side read from
`region_uses`, and `motion.retrigger` copied into `rates.retrigger`, which I added this session without asking whether compatibility mattered. With
them went the rule that a document naming no product is an encoder's "from before products were
named": it is refused, as another plugin's is. Every plugin's state version is 1.

**What is kept is not compatibility support, because it costs nothing:** everything is read by
name, so a key the build lacks is ignored and one the document lacks takes its default. That is how
the format is built ([0058](0058-a-parameter-manifest-is-a-value-a-plugin-owns.md)), and it is what lets a preset made today load after a parameter
is added tomorrow. A document from a newer build still loads as far as it is understood and says so.

**What this costs:** the author's older test sessions load with those four things at their
defaults. Nobody else has a document.

**From the first release on** the format is frozen, the version starts counting, and "write the
loader for version N−1 before there is a version N" (IMPLEMENTATION)
applies again. Until then a format change just changes the format -- and the same goes for a
default: "a new parameter's default must reproduce what was there before" is a rule for shipped
documents, which I had been applying to unshipped ones.

**Not decided here, and raised:** the parameter lists carry an "append only, never reorder" rule for
the same audience, which is why the encoder's has `input.*` and the LFO polarities tacked on the
end. Tidying them is a change to the key lists and waits for an explicit go.
