# 0118 · MIDI: notes trigger envelopes, and a controller is the host's to map

**Status:** accepted

*2026-09-21. Decided by the user, after a proposal of mine that he was right to turn down.*

**What MIDI does in this suite is one thing: a note triggers an envelope.** All three plugins accept
MIDI and produce none. CC, pitch bend, aftertouch and MPE do nothing, and are not planned.

**A MIDI controller moves parameters through the HOST's own learn, not ours.** Every value worth
performing is a host parameter -- every tile, dry and wet, the footer's switches, and the AMOUNT of
every modulation source, which exist for exactly this reason: the matrix's cells are state that grows
and shrinks and so cannot be automated or mapped, and a source's amount is the handle on it that can.
REAPER, Live, Bitwig and the rest map a CC to any plugin parameter, with their own takeover modes,
saved with the project, recorded as automation -- which is what makes it bounce-safe: a controller
is for performing and for writing automation, and the automation is what a render plays. Building a
second learn inside the plugins would duplicate that, per host, for a marker on a tile.

**What I proposed first, and why it was wrong:** CC as a source in the modulation matrix, on the
argument that a CC applied to a host parameter from a message-thread timer does not keep time with
an offline render ([0026](0026-the-link-across-plugins-definitions-travel.md)). True, and beside the point -- nobody bounces a performance from
raw CC; they record it as automation. The matrix route would have cost a state-format and link
version change to solve a problem the host already solves.

**Note learn is every plugin's.** It was greyed in Echo and Reverb, and  said why: "an
effect has no MIDI intake to learn from". It had the intake; what it lacked was forty lines of the
encoder's processor -- arm, catch on the audio thread, take back as an undoable edit -- which are
`PluginProcessor`'s now. The catch walks a block's MIDI only while an envelope is
armed. **Mutation-checked, 4 of 4**, through the button as a user presses it, in Reverb's suite: the
effect never looking for the note, the tick never taking it, the button inert, every note-on
counting instead of the first.

**How a parameter becomes modulatable, since it was asked.** One line: its key in that plugin's
`destOf`, as a rate, an angle or a scalar, which also picks its smoothing. Then a test's expected
list, and -- because the link carries one cell per source and target -- `kLinkMaxCells` and the link
version. That part is mechanical. **What is not is [0049](0049-a-parameter-is-a-modulation-destination-only-if.md)'s rule:** a parameter is a
destination only if its effect is recomputed on the audio thread, in bounded work, from what
`prepare` sized -- a modulated value exists nowhere else. Which parameters ought to be and are not
has not been gone through; it is in [STATUS](../STATUS.md) as something to investigate.
