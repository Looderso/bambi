// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <juce_graphics/juce_graphics.h>

#include "bambi/patch/identity.hpp"

/*  The look of bambi, in one place.

    Every colour, type size, stroke width, corner radius, spacing and layout value the interface
    uses is named here and nowhere else. Code asks for a ROLE -- "the selected source", "a rule" --
    never for a hex value or a pixel size, so a change of look is an edit to this file.
    tools/check-style.sh fails if a literal colour, type size or corner radius turns up anywhere
    else in plugin/Source.

    Two layers of colour:
      palette  the raw colours, named for what they ARE
      colour   what each one is FOR. Repoint a role to change one use; change the palette to
               recolour everything that shares it.

    The direction is Otl Aicher / HfG Ulm: light ground, black line work, structure by rules and
    alignment rather than boxes, no rounding, no shadows, no gradients.
*/
// clang-format off: every value is laid out as a table, which the formatter would flatten.
namespace bambi::ui::theme
{

// ---- palette: Munich 1972 on warm off-white ----------------------------------------------
namespace palette
{
    inline const juce::Colour offWhite  { 0xffefede7 };
    inline const juce::Colour stone     { 0xffe5e2db };
    inline const juce::Colour sandLight { 0xffe1ddd4 };
    inline const juce::Colour sand      { 0xffd5d1c8 };
    inline const juce::Colour greyLight { 0xffc8c4ba };
    inline const juce::Colour greyMid   { 0xffb7b2a8 };
    inline const juce::Colour silver    { 0xffa9a59c };
    inline const juce::Colour greyDark  { 0xff6f6b62 };
    inline const juce::Colour black     { 0xff1b1a17 };
    inline const juce::Colour blue      { 0xff5e8bae };
    inline const juce::Colour blueDeep  { 0xff3f6a8c };
    inline const juce::Colour orange    { 0xffd4873a };
    inline const juce::Colour green     { 0xff7ea47e };
    inline const juce::Colour violet    { 0xff8b7ba8 };
    //  each plugin's own: the encoder's is `blue`
    inline const juce::Colour reverbViolet { 0xff967bb2 };
    inline const juce::Colour echoGreen    { 0xff7cb27b };
} // namespace palette

// ---- colour roles ---------------------------------------------------------------------------
namespace colour
{
    // surfaces
    inline const juce::Colour ground      = palette::offWhite;   ///< the window
    inline const juce::Colour panel       = palette::stone;      ///< globe, equirect, meter well
    inline const juce::Colour highlight   = palette::stone;      ///< selected row, provisional parameter

    // ink
    inline const juce::Colour text        = palette::black;
    inline const juce::Colour label       = palette::greyDark;   ///< lowercase labels, units
    inline const juce::Colour textInverse = palette::offWhite;   ///< on a filled segment or chip

    // structure
    inline const juce::Colour ruleStrong  = palette::black;      ///< section boundaries
    inline const juce::Colour rule        = palette::greyLight;  ///< dividers inside a section
    inline const juce::Colour rowDivider  = palette::sandLight;  ///< between matrix rows
    inline const juce::Colour track       = palette::sand;       ///< the unset part of a control's bar
    inline const juce::Colour barTick     = palette::greyMid;    ///< where a signed value's zero is
    inline const juce::Colour valueFill   = palette::black;      ///< the set part of a control's bar
    inline const juce::Colour liveValue   = palette::orange;     ///< where the engine has the value now

    // the sphere
    inline const juce::Colour sphere        = palette::black;      ///< outline and frame
    inline const juce::Colour graticuleMain = palette::greyMid;    ///< equator, centre lines
    inline const juce::Colour graticule     = palette::greyLight;
    inline const juce::Colour axisLabel     = palette::greyDark;   ///< b · l · f · r
    inline const juce::Colour trajectory    = palette::blue;       ///< the selected instance's path
    inline const juce::Colour source        = palette::orange;     ///< the selected instance's source
    inline const juce::Colour otherInstance = palette::silver;     ///< other instances' paths, sources, labels
    inline const juce::Colour node          = palette::black;      ///< a custom chain's nodes, while editing
    inline const juce::Colour nodeFill      = palette::offWhite;
    inline const juce::Colour handle        = palette::violet;     ///< the selected node's handles and their stems
    inline const juce::Colour insertMark    = palette::orange;     ///< where a click would insert a node
    inline const juce::Colour selectedNode  = palette::orange;     ///< the ring round the selected or hovered node
    inline const juce::Colour width         = palette::orange;     ///< the selected source's width, filled faintly
    /// The added colour: the incoming field is orange and what a plugin adds is blue.
    inline const juce::Colour added         = palette::blue;
    /// and what arrived at the plugin: the other half of the pair.
    inline const juce::Colour sceneIn       = palette::orange;
    /// The ping: a signal sent into the plugin, so it takes the incoming field's orange. The
    /// encoder's orange is its source, which is also what goes in, and the encoder has no ping.
    inline const juce::Colour probe         = palette::orange;

    /// A region is drawn in ink, so colour stays with `in` and `added`. Structure, not signal.
    inline const juce::Colour regionEdge    = palette::black;   ///< its 0.5 contour
    inline const juce::Colour regionWash    = palette::black;   ///< the side that passes
    inline const juce::Colour regionLabel   = palette::black;

    /// What a plugin ANSWERS when the probe is pointed, in as many colours as it has things to
    /// answer with. They step AROUND the added-blue rather than away from it: all one family, none
    /// of them the wash's own colour at full strength.
    inline const std::array<juce::Colour, 4> answerFamily{
        juce::Colour(0xff7f6fa6), juce::Colour(0xff4a7ba0),
        juce::Colour(0xff5f927f), juce::Colour(0xffa2708a)};

    // modulation: its colour is the plugin's own, below
    inline const juce::Colour elsewhere   = palette::greyDark;   ///< "has depth on another matrix tab"
    inline const juce::Colour emptyCell   = palette::stone;

    // state
    inline const juce::Colour inactive    = palette::silver;     ///< unselected selection bar, disabled glyph
    inline const juce::Colour ok          = palette::black;      ///< link healthy
    inline const juce::Colour link        = palette::blue;
    inline const juce::Colour linkHover   = palette::blueDeep;

    // meter
    inline const juce::Colour meterFill   = palette::black;
    inline const juce::Colour meterPeak   = palette::orange;
    inline const juce::Colour meterTick   = palette::greyLight;

    // controls
    inline const juce::Colour chosen      = palette::black;      ///< the open tab, the chosen segment, a toggle that is on, the loaded row of a list; a row that merely selects is `highlight`
    inline const juce::Colour provisional = palette::silver;     ///< a provisional matrix row's dashed outline

    // a source's settings: sources · generators
    inline const juce::Colour envelopeGrid  = palette::greyLight; ///< its axis, and a line at each stage's edge
    inline const juce::Colour learning      = palette::orange;   ///< learn, waiting for a note

    /*  Which plugin this is: the rule under the header and the rule over the footer, and what modulates,
        in the plugin's own colour. */
    inline juce::Colour identity(Product p)
    {
        switch (p)
        {
            case Product::Reverb:  return palette::reverbViolet;
            case Product::Echo:    return palette::echoGreen;
            case Product::Encoder: return palette::blue;
            case Product::Unknown: return palette::blue;
        }
        return palette::blue;
    }

    /*  This plugin's colour, for what it wears besides its rules. Set once by the editor before anything
        paints; one binary is one plugin, so every window in it shares the value. */
    inline juce::Colour& accentSlot()
    {
        static juce::Colour accent = palette::blue;
        return accent;
    }
    inline void setAccent(Product p) { accentSlot() = identity(p); }
    inline juce::Colour modulation() { return accentSlot(); }  ///< cell depth, source amount, modulated range
    inline juce::Colour output()     { return accentSlot(); }  ///< a source's live output, its level and its curve
    inline juce::Colour selected()   { return accentSlot(); }  ///< a selected row's bar, the open source's column
} // namespace colour

// ---- type ----------------------------------------------------------------------------------
//  Neutral grotesque; Univers is the reference. Labels are lowercase throughout, and every number
//  is set in tabular figures.
namespace type
{
    inline constexpr const char* family = "Helvetica Neue";

    //  sizes, px at 1x
    inline constexpr float wordmark   = 15.0f;   ///< "bambi"
    inline constexpr float body       = 11.0f;
    inline constexpr float matrix     = 10.0f;   ///< target names, source column names
    inline constexpr float label      = 9.0f;
    inline constexpr float small      = 8.0f;    ///< sub-labels, axis letters, units under a value
    inline constexpr float value      = 17.0f;   ///< a parameter's value
    inline constexpr float valueShape = 15.0f;   ///< a trajectory shape parameter's value
    inline constexpr float meterValue = 12.0f;   ///< a level's number in the level column

    //  tracking, in em
    inline constexpr float trackingLabel    = 0.14f;
    inline constexpr float trackingTab      = 0.10f;
    inline constexpr float trackingHeading  = 0.20f;
    inline constexpr float trackingWordmark = 0.30f;

    //  the P2 diagnostic readout, monospaced; goes when the real editor arrives
    inline constexpr float readout      = 15.0f;
    inline constexpr float readoutSmall = 12.0f;
} // namespace type

// ---- strokes, px -----------------------------------------------------------------------------
namespace stroke
{
    inline constexpr float rule         = 1.0f;   ///< every rule, outline and frame
    inline constexpr float graticule    = 0.7f;
    inline constexpr float regionEdge   = 1.3f;   ///< a region's 0.5 contour
    inline constexpr float liveOutline  = 1.0f;   ///< where the engine has a region or a path NOW, beside the set one
    inline constexpr float trajectory   = 1.6f;
    inline constexpr float otherPath    = 1.0f;
    inline constexpr float otherPathDash[2] = { 3.0f, 4.0f };   ///< other instances' paths are dashed
    /*  Another instance's region is dotted more tightly than its path: a silver dashed region was
        indistinguishable from a silver dashed path. */
    inline constexpr float foreignRegionDash[2] = { 1.0f, 3.0f };
    inline constexpr float unplacedDash[2] = { 3.0f, 3.0f };   ///< the probe's ring before it is placed
    inline constexpr float toggle       = 1.3f;   ///< an unset toggle square's outline
    inline constexpr float glyph        = 1.3f;   ///< pictograms: 0/45/90 degrees only, one weight, no fills
    inline constexpr float underline    = 2.0f;   ///< a modulated parameter's label
    inline constexpr float identityRule = 2.0f;   ///< the plugin's own rule, under the header and over the footer
    inline constexpr float selectionBar = 3.0f;   ///< an instance row
    inline constexpr float barTick      = 1.0f;   ///< the zero a signed value fills from
    inline constexpr float liveMark     = 2.5f;   ///< where the engine has a value now: over the fill
    inline constexpr float meterPeak    = 2.0f;
    inline constexpr float curve        = 1.8f;   ///< an LFO's wave, an envelope's outline
} // namespace stroke

// ---- shape, px -------------------------------------------------------------------------------
namespace shape
{
    //  Zero on purpose -- no rounded-everything. Kept as a value so it is one edit if that ever
    //  changes, and so nothing rounds a corner by accident.
    inline constexpr float cornerRadius   = 0.0f;
    inline constexpr float sourceDot      = 6.0f;   ///< the selected source, radius
    inline constexpr float otherSourceDot = 4.0f;
    inline constexpr float pairScale      = 0.7f;   ///< a stereo input's second mark, against its instance's dot
    inline constexpr float elsewhereDot   = 2.5f;
    inline constexpr float probeDot       = 4.0f;   ///< the ping, radius: a mark, not a source
    inline constexpr float curvePeak      = 3.0f;   ///< an envelope's peak, radius
} // namespace shape

// ---- the scene ----------------------------------------------------------------------------
namespace scene
{
    inline constexpr int   labelStrip     = 20;      ///< above the sphere: the view's name, and the globe's presets
    inline constexpr int   labelInset     = 9;
    //  a small toggle: in the scene's corner -- `regions always`, the energy switch -- and in
    //  every window's footer, `rates continue` and a plugin's own
    inline constexpr float toggleBox      = 9.0f;    ///< the square itself
    inline constexpr float toggleGap      = 9.0f;    ///< between the square and its label
    inline constexpr float toggleHitPad   = 4.0f;    ///< the hit area reaches past both, so the label clicks
    //  between two toggles in the same strip: wider than `toggleGap` so the pairs read as two
    //  controls and not one row of four things
    inline constexpr float toggleGroupGap = 14.0f;
    inline constexpr int   presetChipPadX = 6;
    inline constexpr int   presetChipPadY = 2;
    inline constexpr float globeRadius    = 0.80f;   ///< of half the smaller side below the strip
    //  the equirect is 2:1 and nothing else, so any other ratio stretches the picture and draws a
    //  round thing as an ellipse; these two are the most it may use of the panel
    inline constexpr float equirectWidth  = 0.95f;   ///< at most, of the panel's width
    inline constexpr float equirectHeight = 0.98f;   ///< at most, of the height below the strip
    inline constexpr float axisLabelGap   = 8.0f;    ///< the equirect's b l f r b, above its frame
    inline constexpr float backAlpha      = 0.35f;   ///< the far hemisphere, drawn fainter
    inline constexpr float energyAlpha    = 0.55f;   ///< the energy wash, under the graticule and the probe
    //  the probe's answer: a bead is a soft patch of sky with a hard core
    inline constexpr float flowAlpha      = 0.34f;   ///< the flow: context, not the thing being read
    inline constexpr float beadBackAlpha  = 0.30f;   ///< a bead round the back, dimmed and never dropped
    inline constexpr float beadFloor      = 0.12f;   ///< the quietest a pass is still drawn at
    inline constexpr float beadMinDeg     = 2.5f;    ///< a point still has a size, or it cannot be seen
    inline constexpr float beadMaxDeg     = 45.0f;
    inline constexpr float beadSpreadDim  = 14.0f;   ///< the same energy spread wider is fainter
    inline constexpr float beadCoreAlpha  = 0.95f;
    inline constexpr float beadMidAlpha   = 0.42f;
    inline constexpr float beadMidStop    = 0.30f;
    inline constexpr float beadCore       = 2.4f;    ///< the hard centre, so a pass stays locatable

    //  a region in the scene
    inline constexpr float energyUnderRegion = 0.5f;   ///< the energy while a region's tab is open; invisible undimmed
    inline constexpr float regionWashAlpha = 0.18f;  ///< the wash where the region's value is 1
    inline constexpr float aimHit          = 13.0f;  ///< hit radius of the aim handle
    inline constexpr float handleHit       = 12.0f;  ///< and of the roll and edge handles
    inline constexpr float rollStemDeg     = 45.0f;  ///< how far from the aim a roll handle stands
    inline constexpr float bandThickAzDeg  = 20.0f;  ///< how far round the thickness handle stands
    inline constexpr float regionLabelGap  = 6.0f;   ///< a closed slot's role label, beside its aim
    inline constexpr float regionLabelWide = 80.0f;
    inline constexpr float regionLabelHigh = 12.0f;
    inline constexpr int   regionCell      = 4;      ///< pixels per sample of a region's contour and wash
    inline constexpr float widthAlpha     = 0.18f;   ///< the selected source's width, where it covers fully
    inline constexpr int   widthCell      = 2;       ///< pixels per sample of the width's coverage
    inline constexpr float strokeTolerance = 0.35f;  ///< pixels: how far leaving a path's point out may move its line
    inline constexpr float strokeMaxChord  = 24.0f;  ///< pixels: the longest straight element a path is drawn with
    inline constexpr std::size_t strokeRunCap = 64;  ///< points one element may stand for, so the check stays cheap
    inline constexpr float hoverLabelGap  = 10.0f;   ///< a hovered source's name, beside its dot

    //  editing a custom chain; hit radii are the core's
    inline constexpr float nodeRadius     = 4.5f;
    inline constexpr float nodeSelected   = 6.0f;
    inline constexpr float nodeEndRing    = 9.5f;    ///< an open path's two derived endpoints
    inline constexpr float nodeHoverRing  = 10.0f;
    inline constexpr float handleRadius   = 4.0f;
    inline constexpr float handleHotGrow  = 1.5f;    ///< a region handle hovered or held, larger by this
    inline constexpr float insertRadius   = 7.0f;
    inline constexpr float insertCross    = 3.0f;
    inline constexpr int   handleStem     = 12;      ///< points in a handle's stem: it follows the sphere
    inline constexpr float noticeInset    = 10.0f;
} // namespace scene

// ---- controls: the matrix and the parameter tabs ---------------------------------------------
namespace controls
{
    //  tabs
    inline constexpr float tabBarHeight  = 27.0f;
    inline constexpr float tabPadX       = 9.0f;   ///< either side of a tab's name: the open one's block
    inline constexpr float tabGap        = 4.0f;   ///< between two tabs' blocks
    inline constexpr float matrixTabsTop = 7.0f;

    //  the right column's content
    inline constexpr float contentTop       = 14.0f;
    inline constexpr float hintGap          = 8.0f;
    inline constexpr float noticeLineHeight = 1.7f;    ///< line height, as a multiple of the type size
    inline constexpr float groupTitleHeight = 14.0f;
    inline constexpr float groupTitleGap    = 11.0f;
    inline constexpr float sectionGap       = 16.0f;
    inline constexpr float segmentHeight    = 24.0f;
    inline constexpr float segmentGap       = 7.0f;
    inline constexpr float listRowHeight    = 23.0f;
    inline constexpr float listPadX         = 9.0f;
    inline constexpr float revealMargin     = 60.0f;   ///< left above and below a control scrolled into view

    //  a value tile: name, number, bar
    inline constexpr float tileColumnGap     = 22.0f;
    inline constexpr float tileRowGap        = 24.0f;
    inline constexpr float tileNameHeight    = 12.0f;
    inline constexpr float tileValueGap      = 4.0f;
    inline constexpr float valueLineHeight   = 1.25f;
    inline constexpr float tileBoxPadX       = 7.0f;   ///< the box around a value: drag it, click it
    inline constexpr float tileBoxPadY       = 6.0f;

    //  Echo's strip: four tap rows on one time axis. Everything here that could come from an
    //  existing token does -- a row is `metrics::matrixRow`, the on/off cell is `segmentHeight`, a
    //  mark is `stroke::liveMark`, the ruler is `tileNameHeight`. These are what is left.
    inline constexpr float stripRowGapX     = 10.0f;   ///< between the cells inside a row; nothing else named one
    inline constexpr float markLevelPower   = 0.45f;   ///< opacity follows the pass's level to this power
    inline constexpr float markFloor        = 0.08f;   ///< under this a mark is not drawn at all
    inline constexpr int   stripZoomMax     = 16;      ///< at full zoom a sixteenth occupies the track a bar did

    //  the matrix
    inline constexpr float matrixHeader    = 62.0f;   ///< its contents end at 60
    inline constexpr float matrixHeadPad   = 7.0f;
    inline constexpr float matrixNameTop   = 4.0f;
    inline constexpr float matrixSubTop    = 16.0f;
    inline constexpr float matrixLiveTop   = 27.0f;   ///< what the source is sending now
    inline constexpr float matrixLiveHeight = 3.0f;
    inline constexpr float matrixAmountTop = 41.0f;   ///< and how much of it the matrix takes
    inline constexpr float matrixAmountGap = 2.0f;
    inline constexpr float cellInset       = 5.0f;
    inline constexpr float cellTextInset   = 4.0f;   ///< a cell's depth, written inside it
    inline constexpr float elsewhereGap    = 5.0f;
    inline constexpr float footerGap       = 12.0f;
    inline constexpr float globalBarWidth  = 110.0f;
    inline constexpr float barHeight       = 7.0f;     ///< every value's bar: one height everywhere
    inline constexpr float liveMarkOver    = 3.0f;     ///< the live mark straddles the bar: 7 px of hairline reads as nothing
    inline constexpr float barGrab         = 8.0f;     ///< and it can be grabbed this far above and below
    inline constexpr float hintAlpha       = 0.4f;     ///< the provisional-row hint, while there is none
    inline constexpr float provisionalDash[2] = { 3.0f, 3.0f };
    inline constexpr float defaultDepth    = 0.5f;     ///< what a click on an empty cell sets

    //  dragging
    inline constexpr float dragPixels    = 200.0f;     ///< a parameter's whole range
    inline constexpr float depthPixels   = 60.0f;      ///< a cell's whole depth
    inline constexpr float fineDrag      = 0.1f;       ///< with shift held
    inline constexpr float stepTolerance = 1.0e-4f;

    //  a source's settings: the temporary tab a matrix column opens
    inline constexpr float closeGlyph         = 7.0f;    ///< the temporary tab's cross
    inline constexpr float stepGlyph          = 6.0f;    ///< prev and next, beside a source's name
    inline constexpr float stepGap            = 8.0f;
    inline constexpr float closeGap           = 7.0f;
    inline constexpr float curveHeight        = 56.0f;   ///< an LFO's wave: glanced at, not dragged
    //  the envelope's graph is taller: `curveHeight` sized a static preview, and a shape you drag
    //  needs more room than one you glance at
    inline constexpr float envelopeHeight     = 150.0f;
    inline constexpr int   envelopeCurvePoints = 24;     ///< samples per bowed stage
    //  what the graph is dragged by: a point at each corner, a diamond at each bowed stage's
    //  midpoint, and a y axis that polarity relabels
    inline constexpr float envelopeGutter     = 32.0f;   ///< room to the left for the axis's numbers
    inline constexpr float envelopeLabelGap   = 6.0f;
    inline constexpr int   envelopeTicks      = 4;       ///< roughly, before they are made nice
    inline constexpr int   lfoTicks           = 3;       ///< its canvas is 56 px: five collide into a stack
    inline constexpr float envelopePoint      = 3.5f;    ///< a corner's radius
    inline constexpr float envelopeHandle     = 3.5f;    ///< a curve handle's half-diagonal
    inline constexpr float envelopeGrab       = 11.0f;   ///< how near a corner counts as on it
    inline constexpr float envelopeLineGrab   = 9.0f;    ///< and a stage's body
    inline constexpr float envelopeBowPixels  = 90.0f;   ///< a whole curve, dragged
    inline constexpr float curveGap           = 12.0f;
    inline constexpr float factRowHeight      = 20.0f;   ///< a calibration row: label left, value right
    inline constexpr float buttonHeight       = 24.0f;
    inline constexpr int   lfoCycles          = 2;
    inline constexpr int   sampleHoldSteps    = 6;
    inline constexpr int   curvePoints        = 64;      ///< per cycle
    inline constexpr float noteDragSpan       = 48.0f;   ///< notes over a whole drag
    inline constexpr float channelDragSpan    = 16.0f;
    inline constexpr float unitDragSpan       = 1.0f;    ///< threshold, velocity
    inline constexpr float hysteresisDragSpan = 0.5f;
    inline constexpr float triggerStep        = 0.01f;
    inline constexpr float handleStep         = 0.002f;   ///< a handle length, in radians
} // namespace controls

// ---- the settings page -------------------------------------------------------------------------
namespace settings
{
    inline constexpr float titleHeight = 40.0f;   ///< "settings", and the cross that closes it
    inline constexpr float columnGap   = 36.0f;   ///< between the shared frame and the plugin's own
    inline constexpr float rowHeight   = 28.0f;   ///< a label and its value -- taller than a calibration's fact row, because one of these holds a field
    inline constexpr float fieldWidth  = 220.0f;  ///< the name's text field
    inline constexpr int   nameLength  = 31;      ///< what the bus's label holds, less its terminator
    inline constexpr float fieldPadX   = 7.0f;    ///< inside the name's field, before its text
    inline constexpr float buttonPadX  = 9.0f;    ///< inside an action's button, either side of its label
    inline constexpr float buttonGap   = 6.0f;    ///< between two framed buttons in a row
} // namespace settings

// ---- the preset browser, over the panel in every plugin -----------------------------------------
//  Its title bar is `settings::titleHeight`, its rows `controls::listRowHeight`, its buttons framed
//  as the settings page's: what is here is only what nothing else has.
namespace presets
{
    inline constexpr float groupHeight = 26.0f;   ///< "factory", "user": a heading with a rule under it
    inline constexpr float indent      = 14.0f;   ///< a folder under its group; a preset under its folder
    inline constexpr float foldBox     = 20.0f;   ///< what the fold chevron is given before a heading's text: the glyph and a gap
    inline constexpr float chipHeight  = 18.0f;   ///< a folder offered under the name being typed
    inline constexpr float chipGap     = 4.0f;
    inline constexpr int   pathLength  = 129;     ///< a folder, a slash and a name
} // namespace presets

// ---- a question over the whole window: the one thing an undo does not bring back -----------------
namespace confirm
{
    inline constexpr float scrim = 0.35f;   ///< how far the window dims under it
    inline constexpr float width = 320.0f;
    inline constexpr float pad   = 18.0f;
    inline constexpr float line  = 20.0f;   ///< the question, and the note under it
} // namespace confirm

// ---- a tooltip: a panel chip with a strong rule, body text, below the pointer ------------------
namespace tooltip
{
    inline constexpr int   delayMs = 600;    ///< how long the pointer rests before it shows
    inline constexpr float padX    = 6.0f;
    inline constexpr float padY    = 3.0f;
    inline constexpr float below   = 18.0f;  ///< from the pointer to the tooltip's top edge
} // namespace tooltip

// ---- the header, in every plugin --------------------------------------------------------------
namespace header
{
    inline constexpr float inset         = 20.0f;
    inline constexpr float gap           = 18.0f;
    inline constexpr float dividerHeight = 16.0f;
    inline constexpr float statusSquare  = 7.0f;
    inline constexpr float glyphBox      = 15.0f;
    //  the plugin's icon, before the wordmark. Reserved and not drawn: each plugin is getting one,
    //  and a square of the right size held open now is what keeps the header from moving when they arrive
    inline constexpr float iconBox       = 16.0f;

    //  The instance tabs, in the middle the header already had spare.
    inline constexpr float tabGap      = 2.0f;
    inline constexpr float tabMinWidth = 62.0f;    ///< narrower than this and a track name says nothing
    inline constexpr float tabMaxWidth = 132.0f;
    inline constexpr float tabTop      = 9.0f;     ///< they sit on the header's bottom edge, to open through it
    inline constexpr float tabPadX     = 8.0f;
    inline constexpr float togglePadX  = 4.0f;     ///< either side of settings' or the preset's block while open
    inline constexpr float tabWheel    = 140.0f;   ///< pixels per wheel notch, when they overflow
    inline constexpr float chevronBox  = 16.0f;    ///< reserved at each end, only while they overflow
    inline constexpr float presetName  = 150.0f;   ///< the most the preset's name is given before it is cut
    inline constexpr float presetStep  = 14.0f;    ///< the preset's chevron box, either side of its name
    inline constexpr float chevronW    = 4.0f;
    inline constexpr float chevronH    = 5.0f;
} // namespace header

// ---- the output meter, in every plugin ---------------------------------------------------------
namespace meter
{
    inline constexpr double floorDb      = -60.0;
    inline constexpr double fallDbPerSec = 24.0;
    inline constexpr double holdSeconds  = 1.5;
    inline constexpr double silenceDb    = -120.0; ///< what a meter reads with nothing through it
    inline constexpr float  valueLabelGap = 2.0f;  ///< between a level's number and its name
    inline constexpr double ticksDb[2]   = { -6.0, -20.0 };
    inline constexpr float  valueTop     = 12.0f;
    inline constexpr float  unitGap      = 10.0f;
    inline constexpr float  footer       = 20.0f;
} // namespace meter

// ---- spacing and layout, px at 1x --------------------------------------------------------------
//  The window is designed at one size and scales uniformly: proportions are designed, not responsive.
namespace space
{
    inline constexpr int grid   = 8;    ///< the modular base
    inline constexpr int inset  = 18;   ///< content inset inside a column
    inline constexpr int gap    = 12;   ///< between the scene panels
    inline constexpr int groupGap = 22; ///< between parameter groups
} // namespace space

namespace metrics
{
    inline constexpr int windowWidth   = 1020;
    inline constexpr int windowHeight  = 600;   ///< the matrix scrolls under its heads
    inline constexpr int headerHeight  = 44;
    inline constexpr int sceneTop      = 8;     ///< from the header to the scene panels
    inline constexpr int footerHeight  = 22;    ///< the matrix's global amount; the panel's switches and readout
    inline constexpr int leftColumn    = 620;   ///< scene above, matrix below: two panels and their insets
    inline constexpr int meterColumn   = 52;
    inline constexpr int meterBar      = 14;
    inline constexpr int scenePanel    = 286;   ///< globe and equirect, both square
    inline constexpr int matrixTargets = 118;   ///< the target-name column
    inline constexpr int matrixRow     = 28;    ///< the cell inside it is 16
    inline constexpr int matrixCell    = 16;
} // namespace metrics

} // namespace bambi::ui::theme
// clang-format on
