// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/patch/undo.hpp"

namespace bambi {

UndoStack::UndoStack(PluginState& target, std::size_t limit) : target_(target), limit_(limit == 0 ? 1 : limit) {}

void UndoStack::push(std::string_view name, std::string_view key) {
    undo_.push_back(Entry{std::string(name), std::string(key), target_, false});
    if (undo_.size() > limit_) undo_.erase(undo_.begin());
    // Any new edit invalidates the redo branch — the standard linear-history rule.
    redo_.clear();
}

void UndoStack::perform(std::string_view name, const Edit& edit) {
    endGesture();
    push(name, {});
    edit(target_);
}

void UndoStack::performWhole(std::string_view name, const Edit& edit) {
    perform(name, edit);
    undo_.back().whole = true;
}

void UndoStack::amendEverywhere(const Edit& edit) {
    edit(target_);
    for (auto& e : undo_) edit(e.before);
    for (auto& e : redo_) edit(e.before);
}

void UndoStack::performCoalescing(std::string_view name, std::string_view key, const Edit& edit) {
    const bool merge = !key.empty() && key == openKey_ && !undo_.empty();
    if (!merge) {
        push(name, key);
        openKey_ = std::string(key);
    } else {
        // Merging: keep the snapshot from the start of the gesture, refresh the label, and
        // still drop the redo branch since this is a new edit.
        undo_.back().name = std::string(name);
        redo_.clear();
    }
    edit(target_);
}

void UndoStack::endGesture() { openKey_.clear(); }

std::string_view UndoStack::undoName() const {
    return undo_.empty() ? std::string_view{} : std::string_view{undo_.back().name};
}

std::string_view UndoStack::redoName() const {
    return redo_.empty() ? std::string_view{} : std::string_view{redo_.back().name};
}

bool UndoStack::undo() {
    if (undo_.empty()) return false;
    endGesture();
    Entry e = std::move(undo_.back());
    undo_.pop_back();
    redo_.push_back(Entry{e.name, e.key, target_, e.whole});
    target_ = std::move(e.before);
    return true;
}

bool UndoStack::redo() {
    if (redo_.empty()) return false;
    endGesture();
    Entry e = std::move(redo_.back());
    redo_.pop_back();
    undo_.push_back(Entry{e.name, e.key, target_, e.whole});
    target_ = std::move(e.before);
    return true;
}

void UndoStack::clear() {
    undo_.clear();
    redo_.clear();
    openKey_.clear();
}

}  // namespace bambi
