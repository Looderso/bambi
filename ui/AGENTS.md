# ui/ — the shared look and components

Every plugin's window is built from what is here; `editor/` composes it into the shared editor. The look and its
reasons: `docs/suite/LOOK.md`, `docs/suite/VISUALISATION.md`.

## Working here

- **`include/bambi/ui/Theme.h` is the one file for the look**: every colour, type size, stroke, radius and spacing is
  a named role there. `tools/check-style.sh` fails on a literal anywhere else.
- **Colour has meaning**, defined once in `Theme.h` and LOOK: what is coming in, what goes out, what is happening now,
  what belongs to another instance, and each plugin's own colour. A new plugin gets its colour as a role there.
- Light ground, black line work, structure by rules and alignment, lowercase labels, tabular figures; no rounding,
  shadows or gradients.
- **One bar for every value; a value is one box** you drag or click. A value shows what you set; a mark shows where
  the engine has it now.
- Press-and-drag splits from click on a **4 px movement threshold**, never on where the click landed.
- Every control edits through an undoable edit committed to the audio thread; a drag is one undo step.
- **A pattern that appears twice is one component** with an option, not two copies. A plugin's window configures the
  shared components; it does not re-draw them.
- The built UI is the reference. Prototypes settle behaviour, never style.
- Every plugin has a picture tool — look at a change rather than assume it.
