// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/editor/EffectFrame.h"

namespace bambi::editor {

void EffectFrame::tick(const host::LinkStatus& link, float outputPeak, int order, float dspPercent) {
    /*  A held handle is re-applied every frame, not only on pointer moves: a region turning under a
        rate otherwise slips from under a still pointer. Does nothing when nothing is held, which is
        almost always. */
    globe.carryHeld();
    equirect.carryHeld();

    /*  The list before the refresh: a selected instance that left the session is let go of here,
        and the refresh below then reads this instance and not the one that is gone. */
    const bool instances = controls.rebuildInstances();
    const bool changed = controls.refresh();
    /*  A region that moved is redrawn, whoever moved it -- a rate, the host, a tile. After the
        refresh, so it is this frame's patch the views are compared against. */
    globe.followRegions();
    equirect.followRegions();
    /*  The probe's answer is the plugin's to the patch as it stands -- Echo's spirals, Reverb's
        reflections -- and is worked out as the views paint. A tap or a room changed, whoever changed
        it, is a picture that changed: the views are asked to paint again. */
    if (changed) {
        globe.repaint();
        equirect.repaint();
    }
    if (instances) parts.header.repaint();

    const bool moved = parts.refresh({{link.open, link.session, link.peers},
                                      controls.target().canUndo(),
                                      controls.target().canRedo(),
                                      juce::String(controls.target().undoName()),
                                      juce::String(controls.target().redoName()),
                                      changed,
                                      outputPeak,
                                      controls.anotherLabel(),
                                      order,
                                      dspPercent});
    /*  The probe's answer is drawn from the patch as it plays: an engine value that moved -- a tap's spin
        under an LFO -- is a picture that moved too. */
    if (moved && !changed) {
        globe.repaint();
        equirect.repaint();
    }
}

}  // namespace bambi::editor
