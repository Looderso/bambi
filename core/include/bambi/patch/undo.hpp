// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "bambi/patch/state.hpp"

namespace bambi {

/*  Snapshot-based undo: every edit goes through here rather than mutating PluginState
 *  directly, so it is undoable by construction. PluginState is a few kilobytes, so a hundred
 *  snapshots cost nothing worth optimising.
 *
 *  UI thread only. Nothing here is real-time safe and nothing here needs to be.
 */
class UndoStack {
public:
    using Edit = std::function<void(PluginState&)>;

    explicit UndoStack(PluginState& target, std::size_t limit = 128);

    /// Apply an edit as one undoable step.
    void perform(std::string_view name, const Edit& edit);

    /// Apply an edit that merges with the previous one when they share a non-empty key. This is
    /// what makes dragging a node one undo step instead of two hundred. Call endGesture() when
    /// the drag finishes, or pass a different key.
    void performCoalescing(std::string_view name, std::string_view key, const Edit& edit);

    /// An edit that replaces the whole patch, host parameters included -- a preset load. Every
    /// other step leaves the parameters to the host; this one, undone or redone, says so, and
    /// whoever owns the parameters sets them from the state.
    void performWhole(std::string_view name, const Edit& edit);
    bool undoIsWhole() const { return !undo_.empty() && undo_.back().whole; }
    bool redoIsWhole() const { return !redo_.empty() && redo_.back().whole; }

    /// Change the patch and every step of its history, as no step of its own: a preset renamed under them.
    void amendEverywhere(const Edit& edit);

    /// Ends any run of coalescing edits. Call on mouse-up.
    void endGesture();

    bool canUndo() const { return !undo_.empty(); }
    bool canRedo() const { return !redo_.empty(); }
    std::string_view undoName() const;  ///< what undo() would revert; empty when none
    std::string_view redoName() const;

    bool undo();
    bool redo();

    /// Forget all history — on preset load, where the previous state is not this document's.
    void clear();

    std::size_t depth() const { return undo_.size(); }
    std::size_t limit() const { return limit_; }

private:
    struct Entry {
        std::string name;
        std::string key;
        PluginState before;
        bool whole{false};
    };

    void push(std::string_view name, std::string_view key);

    PluginState& target_;
    std::vector<Entry> undo_;
    std::vector<Entry> redo_;
    std::size_t limit_;
    std::string openKey_;  ///< key of the run currently coalescing
};

}  // namespace bambi
