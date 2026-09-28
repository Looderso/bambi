// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/ui/EditorFrame.h"

namespace bambi::ui {

bool EditorFrame::refresh(const Facts& now) {
    //  the header follows the bus and the undo stack; nothing else repaints it
    header.setStatus(now.link);
    header.setUndoState(now.canUndo, now.canRedo, now.undoName, now.redoName);
    header.refresh();

    if (now.changed) {
        matrix.repaint();
        page.repaint();
    }
    //  whatever moves with no parameter changing: a source's live output, an angle a rate is turning,
    //  a value the engine moved. Nothing at all when nothing moves.
    const bool moved = page.repaintMoving();
    page.followClipboard();  // what a paste would bring is not in the patch, so no refresh reports it
    settings.refresh();      // nor is the session's peers or the transport, which the open page shows
    matrix.repaintHeader();  // the live source values move every frame; the rows do not

    //  what leaves the plugin, and what is true of the whole instance
    levels.update(now.outputPeak, 1.0 / kFrameHz);
    footer.setStatus(now.instance, now.order, now.dspPercent);
    return moved;
}

}  // namespace bambi::ui
