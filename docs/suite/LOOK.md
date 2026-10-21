# The look and the control rules — shared by every plugin

Every bambi plugin is one family: someone who knows the Encoder should read an effect without learning it again.
This page is the list a new plugin or panel is checked against. The shared code is `ui/` (`Theme.h`, `Tiles.h`,
`ParameterPage.h`, `HostDrag.h`, `StatusFooter.h`, `LevelColumn.h`, `Header.h`); `ui/AGENTS.md` is the short form for
anyone editing it. The Encoder's own window is [encoder/UI.md](../encoder/UI.md).

## Visual system

- **One file for the look.** Every colour, type size, stroke, radius and spacing is a named role in
  `ui/include/bambi/ui/Theme.h`, in the shared `bambi-ui` library. `tools/check-style.sh` fails on a literal anywhere
  in `ui/`, `editor/` or `plugins/`. A second plugin links it; it never copies it. Colours come in two layers: a
  palette of raw colours and roles saying what each is for, so a change of look is an edit to that file.
- **Ulm school:** light ground, black line work, structure by rules and alignment on an 8 px grid, lowercase labels,
  tabular figures, a neutral grotesque, no rounding, no shadows, no gradients, no glow. Pictograms use 0/45/90° only,
  one stroke weight, no fills.
- **Colour has meaning, not decoration.** **Orange is in, blue is out** across the suite. Each plugin has its own
  colour, along the signal: the encoder orange, Echo sage, Reverb violet, the decoder blue. It sits on the rules under
  the header and over the footer (2 px) and on modulation, a source's signal and a selection. **Grey is what is
  happening now**: the live mark on a bar, the meter's peak, the selected node's ring, learn waiting. What is held
  stays ink. **Silver is other instances.** Paths and regions are structure, drawn in ink. In the scene orange is what
  came in and blue is what this plugin adds ([VISUALISATION](VISUALISATION.md)); these are separate roles, so
  repointing one never moves the other.
- **Naming.** A trajectory's shapes are *shapes*. *Generators* are LFOs and envelopes and nothing else.

## Values

- **One bar for every value** (`drawValueBar`): 7 px high, one fill rule, one place to change it. A **signed** value
  fills from its own zero with a tick marking it, so a centred value reads as centred; an unsigned one fills from the
  left. Matrix cells fill along the long side from the middle (left negative, right positive) with the depth written
  in the cell. A modulation's reach draws over the fill in the modulation colour.
- **A value is one box** (`addParameterTile`, `addHostDrag`). Drag anywhere in it to set it, up or right to
  increase; click it to give it a matrix row. **A click on the bar itself** (the bar and a 5 px margin above and below)
  puts the value where it was clicked. Sideways a drag moves at the bar's width so the fill stays under the pointer;
  vertically it is finer (200 px a range). Shift is fine. A click on a value that takes no modulation steps it to its
  next option. Double-click restores the default. Drag and click split on a 4 px movement threshold, never on where the
  click landed.
- **A value says what you set; a mark says where the engine has it now.** The number you drag is the parameter: what
  automation records and undo restores. Where the engine applies something else (a rate has turned an angle, a path
  setting is modulated) a **live mark** in the live colour straddles the bar, built for the eye rather than clipped
  into the bar. With nothing moving it, no mark is drawn: an indicator that is always on says nothing. Which
  parameters have one is the plugin's (`ParameterPage::liveValue`); `drawTile` returns the region it drew so the
  repaint covers exactly the mark, and only that region repaints at frame rate. The same rule draws a path or region
  as set with a thin live-colour outline where the engine has it.
- **A value that is one turn wraps**: yaw, pitch and roll, a region's angles, Echo's tap azimuth and spin, an LFO's
  phase, displace, a shape's azimuths, phase and heading. Dragged past an end it carries on from the other; modulated
  past an end it returns the short way round. A value with real ends stops at them. A host automation lane cannot wrap,
  so a drag across the seam writes a jump into it, which plays back right.
- **Double-clicking an angle returns it to exactly zero.** A rate's integrated turn is added to an angle, not written
  into it, so the page clears the turn with the parameter (`ParameterPage::onDefaultRestored`, which reads from the
  key which parameters are region angles; a plugin adds its own and sends the command to its audio thread). A scene
  handle double-clicked back to default clears the turn itself. Displace is the same kind of value: its default
  clears the distance travelled. The turn is engine state, so clearing it is not undoable; from another window it
  travels as a link command. Double-clicking a rate only sets the rate to 0.
- **Every control edits through an undoable edit committed to the audio thread; a drag is one undo step.**

## Choices, tabs and labels

- **One sign for chosen: a black block.** Every tab (instances, the panel's, the matrix's, a sub-tab), every segmented
  choice, the scene's view presets, and settings and the preset browser while open: names in black, the chosen one a
  black block with light text. No frame, no lines between options, no underline. Grey is only for what cannot be
  chosen. A toggle is filled when on and outlined in black when off.
- **A row can act as a tab, and a control inside it outranks the row.** A row that selects something takes
  `colour::highlight` with a `stroke::selectionBar` in the plugin's colour down its left edge, never a black fill. A
  click on a value in the row selects it; a drag edits and never selects; a control that silences or removes
  something takes no selecting click. A value in a row keeps its box.
- **The component is uniform; the label follows the quantity.** The shared control, its interaction and its drawing
  rule are the same at every call site, so a difference in what a number means shows in the name only. A tile is named
  for what its number *is*, and a tile name is unique within its panel.
- **Free or synced draws both timings**, the unused one greyed and inert, never replaced. **Swing takes its own row**
  beneath the pair, greyed when free. In a pair the free tile names the quantity and the synced tile the musical unit:

  | | free | synced |
  |---|---|---|
  | LFO timing | `rate` (Hz) | `division` |
  | Echo tap period | `time` (ms) | `steps` |
  | Echo tap offset | `offset` (ms) | `offset steps` |

- **Grey what one click restores; remove what the user can never make relevant.** A closed/open switch on a custom
  path says nothing a user can change, so it is not shown.
- **A repeated pattern is one component.** A near-copy is a defect: it grows an option, or the difference becomes a
  decision. A UI ported from a prototype matches the prototype's layout; prototypes settle behaviour, never style.

## Pages

- **A page opens with its main selector**, with no title or description above it: the trajectory's shape, a source's
  input mode, a sub-tab row, an LFO's wave, a region's kind. Names say what a choice is for.
- **State left, rate right.** A value and the rate that moves it share a row. The one exception is the decoder's
  orientation strip, wide and short, with three states over their three rates.
- **A selector that wraps is one grid**: its lines share an edge with no gap.
- **Three distances.** `segmentGap` (7) between rows of one thing: a selector and the selector under it, a line of
  text under its control. `sectionGap` (16) between a selector and the controls it governs. `groupGap` (22) before a
  group title. A group title brings its rule, so a divider marks a change of subject, never a change of control. A
  tile's box reaches `tileBoxPadY` past `tilesBottom`, which the two larger gaps cover and `segmentGap` does not.
- **No tile is ever full width.** A page that needs an odd one out leaves the other column empty.
- **A column head shows what its source is sending**: a thin live bar in the source's own range above the amount bar.

## Frame

- **Header:** wordmark, the plugin's name, undo and redo, **instances as tabs** of that plugin only, then preset, link
  and settings. The selected tab is a black block standing on the header's rule in the plugin's colour.
- **Scene top left:** globe and equirect, square panels. **Matrix bottom left**, the same component in every plugin;
  each plugin offers the sources that make sense in it.
- **Right column:** the plugin's own tabs, and under them the **footer** (instance-wide switches on the left, the
  readout on the right: the instance shown, the order, `dsp`). **Level column** far right: the plugin's levels over its
  output meter. A level in the column is not also a tile on a page. Both are the same component in every plugin.
- **One window size per plugin, scaled uniformly** by `editor::PluginEditor`.
- **Panels scroll.** The tab bar and the matrix's column heads stay put and their contents scroll under them: heads that
  scrolled away would leave a depth in a column you can no longer name. Each painter receives the full height it would
  have had and lays itself out whole; the viewport decides what shows (`ParameterPage::paintFrame` sets it and returns
  the scrolled content rect). Hit-testing uses the raw event position against regions rebuilt on every paint at the
  coordinates they are drawn at, so drawing and click targets move together with no second coordinate set. Everything
  that lays content out reports its bottom, and the scroll position is clamped after every paint
  (`HitArea::paintOverChildren`), because a panel that no longer overflows takes no wheel. The slack past the
  content applies only once content overflows; a page that fits never scrolls.
- **Margins carry nothing.** `space::gap` also sets the scene panels' horizontal gap and so the left column's width;
  other margins are sized to their contents (a footer holds a label and a 7 px bar).
- **Settings is not a tab:** a page that opens from the header's glyph, the same frame in every plugin: the instance's
  name, the link session, the order and the host's reported buses, with the plugin's own beside it. The footer and the
  level column stay in front of it. Presets are the header's `‹ preset · name * ›` and a browser over the panel
  ([PRESETS](PRESETS.md)).
- **A run of channels is cells of one size** (`ui/ChannelStrip`). In one row, grouped, on the Decoder's map: wider
  than the space, it scrolls sideways with the wheel under a thin bar, and the shown output is brought into view. In
  lines that wrap on the live host's device cards, where the page has room for every channel. An empty cell carries
  its number, a placed one the channel it plays (L, R, 1…). Never a dropdown, never cells that shrink.
  Every cell is painted by the strip: the panel's ground, or its owner's ink; dashed for the host's own copy or an
  output's own channel while it does not play; two things on one, framed strong with their count.
- **A selected row** -- the one whose settings a panel shows -- is the highlight with the selection bar down its left
  edge (`ui::drawSelectedRow`); the loaded row (the current preset) is ink; a row under the pointer, the highlight
  alone.
- **A value where a tile does not fit** (a tap's level, a loudspeaker's angle) is a value cell: the number on the
  panel's ground in the matrix's type, dragged as a tile's box is (`ui::drawValueCell`); a selected row's ground
  stops before it. Every value drags the same way, across or up, finer with shift.
- **Beats and bars are one ruler** (`ui::drawBeatRuler`): a numbered bar's tick half its height, every other a
  quarter, rising from its foot, or hanging from its top under what it measures.
- **A toggle's square** carries a number when it stands for one (a tap), and **a tag** is a chip that offers
  nothing: what a thing is, strong while it is in play (`ui::drawToggleBox`, `ui::drawTag`).
- **A text field is a value's box with a caret.** There is one, the instance's name: the panel's ground, the rule's
  outline going strong when it has the keyboard, no menu of JUCE's own. Return and escape give the keyboard back,
  because the next key after naming a track is the host's space bar.
- **A rotatable view reserves drag for the camera**; a flat view does not. Projection and view are separate: one
  component, N projections.

Plugin pages that follow this frame: [echo/ECHO.md](../echo/ECHO.md), [reverb/REVERB.md](../reverb/REVERB.md),
[decoder/DECODER.md](../decoder/DECODER.md). What every plugin draws on the sphere: [VISUALISATION](VISUALISATION.md).

Background: decisions 0139.
