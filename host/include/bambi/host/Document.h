// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>

#include "bambi/patch/parameters.hpp"
#include "bambi/patch/state.hpp"
#include "bambi/patch/undo.hpp"

/*  The patch as the user has it, and its history.
 *
 *  Every plugin carries one, and every plugin changes it the same way: an edit runs against the
 *  editing copy under a lock, and the result is committed whole to the audio thread through the
 *  handoff. Nothing real-time touches any of this.
 *
 *  Two copies, deliberately:
 *
 *    - the editing copy, which the undo stack owns and every edit runs against. A host may load
 *      state from a thread of its own, hence its lock.
 *    - the committed copy, which state is saved from and the bus publishes, under a lock of its own
 *      so that two commits racing from two threads cannot leave the audio thread on the older one.
 *
 *  `commit` is the plugin's: it builds a snapshot and posts it, which is the one part that knows
 *  what an engine wants.
 */
namespace bambi::host {

class Document {
public:
    /*  What a commit does. Called with the new state and whether it is the whole patch, host
     *  parameters included -- a preset loaded, or that load undone or redone. Then the plugin sets
     *  its parameters from the state and the engine snaps; otherwise the parameters in the state are
     *  not the host's and nothing reads them. */
    using Commit = std::function<void(PluginState, bool)>;
    /// Bring a state's parameters up to the host's, so that a whole step's "before" is what was heard.
    using Live = std::function<void(PluginState&)>;

    Document(const ParamManifest& m, Commit commit, Live live = {})
        : editState_(m), committed_(m), undo_(editState_), commit_(std::move(commit)), live_(std::move(live)) {}

    // ---- the editing copy -------------------------------------------------------------------
    /// One undoable edit, seen at once.
    void edit(std::string_view name, const UndoStack::Edit& change) {
        PluginState next = run([&](UndoStack& u) { u.perform(name, change); });
        commit_(std::move(next), false);
    }

    /*  One step of a drag: every call with the same key is the same undo step until endGesture.
     *  The key carries the sender, so one window's drag is never coalesced with another's. */
    void editCoalescing(std::string_view name, std::string_view key, const UndoStack::Edit& change) {
        PluginState next = run([&](UndoStack& u) { u.performCoalescing(name, key, change); });
        commit_(std::move(next), false);
    }

    /// One undoable edit that replaces the whole patch, parameters and all: a preset load. The parameters are read from the host first, so undoing it puts back what was there.
    void editWhole(std::string_view name, const UndoStack::Edit& change) {
        PluginState next = run([&](UndoStack& u) {
            if (live_) live_(editState_);
            u.performWhole(name, change);
        });
        commit_(std::move(next), true);
    }

    /// A change that is no step of the history: the name of the preset a patch was just saved as. `inHistory` makes it in every step as well -- a preset renamed is renamed wherever it was.
    void amend(const UndoStack::Edit& change, bool inHistory = false) {
        PluginState next = run([&](UndoStack& u) {
            if (inHistory)
                u.amendEverywhere(change);
            else
                change(editState_);
        });
        commit_(std::move(next), false);
    }

    void endGesture() {
        const std::lock_guard<std::mutex> lock(editMutex_);
        undo_.endGesture();
    }

    bool undo() { return step(true); }
    bool redo() { return step(false); }

    bool canUndo() const {
        const std::lock_guard<std::mutex> lock(editMutex_);
        return undo_.canUndo();
    }
    bool canRedo() const {
        const std::lock_guard<std::mutex> lock(editMutex_);
        return undo_.canRedo();
    }
    /// What undo and redo would revert or reapply; empty when there is nothing.
    std::string undoName() const {
        const std::lock_guard<std::mutex> lock(editMutex_);
        return std::string(undo_.undoName());
    }
    std::string redoName() const {
        const std::lock_guard<std::mutex> lock(editMutex_);
        return std::string(undo_.redoName());
    }

    /// A copy of the editing state: what an editor draws and what the bus publishes as the patch.
    PluginState editing() const {
        const std::lock_guard<std::mutex> lock(editMutex_);
        return editState_;
    }

    /// Read it under the lock without copying it: for packing, sixty times a second.
    template <class Fn>
    auto withEditing(Fn&& fn) const {
        const std::lock_guard<std::mutex> lock(editMutex_);
        return fn(static_cast<const PluginState&>(editState_), static_cast<const UndoStack&>(undo_));
    }

    /// The state a fresh instance starts from, before anything can have edited it. Both copies at once, and no sequence is spent: the first real commit is still number 1.
    void initialise(const PluginState& start) {
        const std::lock_guard<std::mutex> edits(editMutex_);
        const std::lock_guard<std::mutex> commits(committedMutex_);
        editState_ = start;
        committed_ = start;
        undo_.clear();
    }

    /// A loaded state is a new document: the editor starts from it, and the history before it is not this document's. Does not commit -- the caller does, since only it knows whether the engine should snap.
    void adopt(const PluginState& loaded) {
        const std::lock_guard<std::mutex> lock(editMutex_);
        editState_ = loaded;
        undo_.clear();
    }

    // ---- the committed copy -------------------------------------------------------------------
    /*  Take the new state as committed. `underLock` is given this commit's sequence number and the
     *  state, and runs with the lock held -- so the document, its sequence and the handover to the
     *  audio thread all move together, and two commits racing from two threads cannot leave the
     *  audio thread on the older one. The audio thread never takes this lock. */
    template <class Fn>
    void takeCommitted(PluginState&& next, Fn&& underLock) {
        const std::lock_guard<std::mutex> lock(committedMutex_);
        underLock(nextSequence_++, next);
        committed_ = std::move(next);
    }

    PluginState committed() const {
        const std::lock_guard<std::mutex> lock(committedMutex_);
        return committed_;
    }

    /// What the next commit will be numbered. A change to it is how a window knows the patch moved.
    std::uint64_t nextSequence() const {
        const std::lock_guard<std::mutex> lock(committedMutex_);
        return nextSequence_;
    }

    std::mutex& committedMutex() const noexcept { return committedMutex_; }

private:
    template <class Fn>
    PluginState run(Fn&& fn) {
        const std::lock_guard<std::mutex> lock(editMutex_);
        fn(undo_);
        return editState_;
    }

    bool step(bool back) {
        PluginState next;
        bool whole = false;
        {
            const std::lock_guard<std::mutex> lock(editMutex_);
            whole = back ? undo_.undoIsWhole() : undo_.redoIsWhole();
            //  what the other direction will restore is what the host has now, not what was loaded
            if (whole && live_) live_(editState_);
            if (back ? !undo_.undo() : !undo_.redo()) return false;
            next = editState_;
        }
        commit_(std::move(next), whole);
        return true;
    }

    mutable std::mutex editMutex_;
    PluginState editState_;  ///< under editMutex_
    mutable std::mutex committedMutex_;
    PluginState committed_;          ///< under committedMutex_
    std::uint64_t nextSequence_{1};  ///< under committedMutex_
    UndoStack undo_;                 ///< under editMutex_; refers to editState_
    Commit commit_;
    Live live_;
};

}  // namespace bambi::host
