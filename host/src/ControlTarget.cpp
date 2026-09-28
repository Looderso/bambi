// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/host/ControlTarget.h"

#include <algorithm>
#include <cmath>

namespace bambi::host {
namespace {
/// How long an edit sent to another instance is shown before the controls go back to what it publishes,
/// if it never reports applying it.
constexpr double kPendingSeconds = 1.0;
/// How long a released parameter shows the value sent while the target's own value catches up.
constexpr double kHeldSeconds = 0.5;

std::size_t at(ParamId id) { return static_cast<std::size_t>(id); }

float toPlain(const ParamManifest& m, ParamId id, float normalised) {
    //  The host parameter's own mapping, skew included: the target converts this value back through it.
    const auto position = static_cast<int>(id);
    return clampToRange(m, position, fromNormalised(m, position, std::clamp(normalised, 0.0f, 1.0f)));
}
}  // namespace

void ControlTarget::changeParameter(ParamId id, float normalised) {
    beginParameter(id);
    setParameter(id, normalised);
    endParameter(id);
}

// ---- this instance ---------------------------------------------------------------------------

PluginState LocalTarget::state() const {
    //  Host parameters live in the host's parameters, not in the document: read them live, so a
    //  value on screen and a modulation reach are what the host has.
    auto& link = host_.linkHost();
    PluginState s = link.document().editing();
    const auto count = std::min(static_cast<std::size_t>(link.manifest().size()), s.params.size());
    for (std::size_t i = 0; i < count; ++i) s.params[i] = link.parameterValue(static_cast<int>(i));
    return s;
}

void LocalTarget::edit(std::string_view name, const UndoStack::Edit& change) {
    host_.linkHost().document().edit(name, change);
    host_.linkNode().noteActivity();
}

void LocalTarget::editDrag(std::string_view name, std::string_view key, const UndoStack::Edit& change) {
    host_.linkHost().document().editCoalescing(name, key, change);
}

void LocalTarget::endDrag() {
    host_.linkHost().document().endGesture();
    host_.linkNode().noteActivity();
}

void LocalTarget::beginParameter(ParamId id) {
    if (auto* param = host_.linkHost().hostParameter(id)) param->beginChangeGesture();
}

void LocalTarget::setParameter(ParamId id, float normalised) {
    if (auto* param = host_.linkHost().hostParameter(id))
        param->setValueNotifyingHost(std::clamp(normalised, 0.0f, 1.0f));
}

void LocalTarget::endParameter(ParamId id) {
    if (auto* param = host_.linkHost().hostParameter(id)) param->endChangeGesture();
    host_.linkNode().noteActivity();
}

bool LocalTarget::canUndo() const { return host_.linkHost().document().canUndo(); }

bool LocalTarget::canRedo() const { return host_.linkHost().document().canRedo(); }

std::string LocalTarget::undoName() const { return host_.linkHost().document().undoName(); }

std::string LocalTarget::redoName() const { return host_.linkHost().document().redoName(); }

void LocalTarget::undo() { host_.linkHost().document().undo(); }

void LocalTarget::redo() { host_.linkHost().document().redo(); }

float LocalTarget::sourceValue(int slot) const { return host_.liveSourceValue(slot); }

int LocalTarget::learning() const { return host_.noteLearnArmed(); }

void LocalTarget::learn(int envelope) { host_.linkHost().armNoteLearn(envelope); }

void LocalTarget::zeroTurn(int axis) { host_.linkHost().zeroPlacementTurn(axis); }

void LocalTarget::zeroRegionTurn(int slot, int axis) { host_.linkHost().zeroRegionTurn(slot, axis); }

// ---- another instance --------------------------------------------------------------------------

RemoteTarget::~RemoteTarget() { release(); }

bool RemoteTarget::takeEnergy(std::vector<float>& added, std::vector<float>& arrived, int& order) {
    if (energyGeneration_ == 0 || energyGeneration_ == energyTaken_) return false;
    energyTaken_ = energyGeneration_;
    order = energy_.order;
    const auto n = static_cast<std::ptrdiff_t>(energy_.count);
    added.assign(energy_.added, energy_.added + n);
    if (energy_.withArrived != 0)
        arrived.assign(energy_.arrived, energy_.arrived + n);
    else
        arrived.clear();
    return true;
}

void RemoteTarget::follow(const LinkScene::Entry& peer, double dt) {
    if (!(peer.instance == instance_)) {
        release();
        instance_ = peer.instance;
        generation_ = 0;
        hasPending_ = false;
        held_ = {};
        energyGeneration_ = energyTaken_ = 0;
    }
    if (energyWanted_) {
        //  Asked a second ahead, every frame: the owner gathers only while somebody keeps asking.
        auto& node = host_.linkNode();
        node.askForEnergy(peer, linkNowMicros() + 1'000'000);
        std::uint32_t generation = 0;
        if (node.readEnergy(peer, scratchEnergy_, generation) && generation != energyGeneration_) {
            energy_ = scratchEnergy_;
            energyGeneration_ = generation;
        }
    }
    //  Only as many sources as this build has: the bus reserves more (`kLinkMaxSources`) than a
    //  build uses (`kNumSources`), and copying the whole array overruns `sources_`.
    std::copy_n(std::begin(peer.dyn.sources), sources_.size(), sources_.begin());

    if (peer.controlsGeneration != 0 && peer.controlsGeneration != generation_) {
        //  Into scratch first: a failed read leaves its copy unspecified.
        std::uint32_t generation = 0;
        if (host_.linkNode().readControls(peer, scratch_, generation)) {
            controls_ = scratch_;
            generation_ = generation;
        }
    }

    //  An edit shows until the target has applied it -- sequences only grow, so a later one covers it -- or
    //  until it has plainly been lost.
    if (hasPending_ && (controls_.appliedEdit >= pendingSequence_ || (pendingSeconds_ += dt) > kPendingSeconds))
        hasPending_ = false;

    for (std::size_t i = 0; i < held_.size(); ++i) {
        auto& h = held_[i];
        if (!h.active || h.open) continue;
        const auto& d = host_.linkHost().manifest()[static_cast<int>(i)];
        const bool arrived =
            i < controls_.paramCount && std::abs(controls_.params[i] - h.plain) <= 1.0e-4f * (d.max - d.min);
        if (arrived || (h.seconds += dt) > kHeldSeconds) h.active = false;
    }
}

void RemoteTarget::release() {
    if (instance_.isNil()) return;
    for (std::size_t i = 0; i < held_.size(); ++i)
        if (held_[i].open) endParameter(static_cast<ParamId>(i));
    endDrag();
}

PluginState RemoteTarget::state() const {
    PluginState s{host_.linkHost().manifest()};
    unpackPatch(host_.linkHost().modManifest(), hasPending_ ? pending_ : controls_.patch, s);
    const auto count = std::min(static_cast<std::size_t>(controls_.paramCount), s.params.size());
    for (std::size_t i = 0; i < count; ++i) s.params[i] = controls_.params[i];
    for (std::size_t i = 0; i < held_.size(); ++i)
        if (held_[i].active) s.params[i] = held_[i].plain;
    return s;
}

void RemoteTarget::send(std::string_view name, std::string_view key, const UndoStack::Edit& change) {
    auto next = state();
    change(next);
    packPatch(next, pending_);
    if (const auto sequence = host_.linkNode().sendPatchEdit(instance_, pending_, name, key, false); sequence != 0)
        pendingSequence_ = sequence;
    hasPending_ = true;
    pendingSeconds_ = 0.0;
    host_.linkNode().noteActivity();
}

void RemoteTarget::edit(std::string_view name, const UndoStack::Edit& change) { send(name, {}, change); }

void RemoteTarget::editDrag(std::string_view name, std::string_view key, const UndoStack::Edit& change) {
    dragging_ = true;
    dragName_ = std::string(name);
    dragKey_ = std::string(key);
    send(name, key, change);
}

void RemoteTarget::endDrag() {
    if (!dragging_) return;
    dragging_ = false;
    //  The last state again, marked as the end, so the target closes this run of its undo stack.
    const auto sequence =
        host_.linkNode().sendPatchEdit(instance_, hasPending_ ? pending_ : controls_.patch, dragName_, dragKey_, true);
    if (sequence != 0 && hasPending_) {
        pendingSequence_ = sequence;
        pendingSeconds_ = 0.0;
    }
    host_.linkNode().noteActivity();
}

float RemoteTarget::plainValue(ParamId id) const {
    const auto i = at(id);
    if (held_[i].active) return held_[i].plain;
    return i < controls_.paramCount ? controls_.params[i] : host_.linkHost().manifest()[static_cast<int>(id)].def;
}

void RemoteTarget::beginParameter(ParamId id) {
    auto& h = held_[at(id)];
    h.plain = plainValue(id);
    h.active = h.open = true;
    h.seconds = 0.0;
    host_.linkNode().sendParameter(instance_, id, h.plain, kLinkGestureBegin);
}

void RemoteTarget::setParameter(ParamId id, float normalised) {
    auto& h = held_[at(id)];
    h.plain = toPlain(host_.linkHost().manifest(), id, normalised);
    h.active = true;
    h.seconds = 0.0;
    //  Outside a gesture the target brackets it as one change (LinkEditReceiver).
    host_.linkNode().sendParameter(instance_, id, h.plain, kLinkGestureNone);
}

void RemoteTarget::endParameter(ParamId id) {
    auto& h = held_[at(id)];
    h.plain = plainValue(id);
    h.open = false;
    h.seconds = 0.0;
    host_.linkNode().sendParameter(instance_, id, h.plain, kLinkGestureEnd);
    host_.linkNode().noteActivity();
}

void RemoteTarget::undo() { host_.linkNode().sendCommand(instance_, kLinkCommandUndo, 0.0f); }

void RemoteTarget::redo() { host_.linkNode().sendCommand(instance_, kLinkCommandRedo, 0.0f); }

float RemoteTarget::sourceValue(int slot) const {
    return sources_[static_cast<std::size_t>(std::clamp(slot, 0, kNumSources - 1))];
}

void RemoteTarget::learn(int envelope) {
    host_.linkNode().sendCommand(instance_, kLinkCommandLearn, static_cast<float>(envelope));
}

void RemoteTarget::zeroTurn(int axis) {
    host_.linkNode().sendCommand(instance_, kLinkCommandZeroTurn, static_cast<float>(axis));
}

void RemoteTarget::zeroRegionTurn(int slot, int axis) {
    //  slot and axis packed into one float, as the command expects
    host_.linkNode().sendCommand(instance_, kLinkCommandZeroRegionTurn,
                                 static_cast<float>(slot * kZeroRegionTurnAxes + axis));
}

}  // namespace bambi::host
