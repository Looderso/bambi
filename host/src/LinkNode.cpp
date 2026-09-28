// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/host/LinkNode.h"

#include <algorithm>
#include <cmath>

namespace bambi::host {

std::string& linkDirectoryName() {
    static std::string name{kLinkDirectoryName};
    return name;
}

void setLinkDirectoryNameForTests(std::string name) { linkDirectoryName() = std::move(name); }

LinkNode::~LinkNode() {
    if (bus_.isOpen() && bus_.slotCount() <= 1) directory_.remove(host_.identity().session);
}

void LinkNode::tick() {
    /*  The first join waits a few ticks. A project that is loading sets this instance's state right
     *  after constructing it, and the state carries the session; joining at once would guess a
     *  session and then move. */
    if (!adopted_ && --ticksBeforeFirstJoin_ <= 0) {
        adopted_ = true;
        join();
    } else if (adopted_ && !bus_.isOpen() && --ticksUntilRetry_ <= 0) {
        /*  Joining was attempted and this instance is not on the bus: shared memory was
         *  unavailable, or a rejoin found this instance id live elsewhere. */
        if (bus_.instanceConflict())
            host_.adoptIdentity(host_.identity().session, Uuid::nil());  // join takes a fresh one
        join();
    }

    if (bus_.isOpen()) {
        if (staticDirty_) {
            staticDirty_ = false;
            bus_.publishStatic(buildStatic());
        }
        publisher_.tick();
        directory_.heartbeat(host_.identity().session, linkNowMicros());

        /*  Remote edits: drained, made well-formed by the receiver -- which also ends the gestures
         *  of senders that are gone -- and applied through this instance's own host parameters, so
         *  they automate and undo exactly like the user's own. Another window's patch edit first --
         *  the whole patch, latest wins -- then undo, redo and the rest, in the order they were sent
         *  after it. */
        if (bus_.takePatchEdit(incoming_, lastPatchEdit_)) applyPatchEdit();
        bus_.drainCommands(commands_);
        for (const auto& command : commands_)
            if (command.param >= kLinkCommandUndo) {
                switch (command.param) {
                    case kLinkCommandUndo: host_.document().undo(); break;
                    case kLinkCommandRedo: host_.document().redo(); break;
                    case kLinkCommandLearn: host_.armNoteLearn(static_cast<int>(std::lround(command.value))); break;
                    case kLinkCommandZeroTurn:
                        host_.zeroPlacementTurn(static_cast<int>(std::lround(command.value)));
                        break;
                    case kLinkCommandZeroRegionTurn: {
                        const int packed = static_cast<int>(std::lround(command.value));
                        if (packed >= 0)
                            host_.zeroRegionTurn(packed / kZeroRegionTurnAxes, packed % kZeroRegionTurnAxes);
                        break;
                    }
                    default: host_.applyCommand(command); break;
                }
            }
        actions_.clear();
        receiver_.apply(commands_, actions_);
        bus_.poll(peers_);
        receiver_.closeAbandoned(peers_, actions_);
        for (const auto& action : actions_) {
            auto* param = host_.hostParameter(action.param);
            if (param == nullptr) continue;
            switch (action.kind) {
                case LinkEditAction::Kind::Begin: param->beginChangeGesture(); break;
                case LinkEditAction::Kind::Value:
                    param->setValueNotifyingHost(juce::jlimit(0.0f, 1.0f, param->convertTo0to1(action.value)));
                    ++status_.remoteEdits;
                    break;
                case LinkEditAction::Kind::End: param->endChangeGesture(); break;
            }
        }

        publishControls();

        if (!bus_.isOpen() && bus_.instanceConflict())
            ticksUntilRetry_ = 0;  // a rejoin just stepped back: take a fresh id on the next tick
    }
    updateStatus();
}

void LinkNode::join() {
    ticksUntilRetry_ = 2 * kTickHz;
    const auto now = linkNowMicros();
    Identity id = host_.identity();

    //  A saved instance brings its session. A new one joins the session the user was most recently
    //  active in, or starts one when none is live.
    if (id.session.isNil()) id.session = sessionForNewInstance(directory_, now, Uuid::generate());
    if (id.instance.isNil()) id.instance = Uuid::generate();

    bool joined = bus_.open(id.session, id.instance, host_.product());
    if (!joined && bus_.instanceConflict()) {
        /*  A duplicated track or a pasted instance: the original is live with this id. It keeps it;
         *  this copy becomes a new instance, and saves as one. */
        id.instance = Uuid::generate();
        joined = bus_.open(id.session, id.instance, host_.product());
    }

    host_.adoptIdentity(id.session, id.instance);
    if (!joined) return;

    LinkDynamic seed;
    host_.seedDynamic(seed);
    publisher_.seed(seed);

    staticDirty_ = true;
    controlsPublished_ = false;
    directory_.touch(id.session, now);  // joining is activity: this is where the user is working
}

void LinkNode::publishControls() {
    /*  Compared field by field, not rebuilt: sixty times a second, nothing is packed or published
     *  unless something in it changed. */
    bool changed = !controlsPublished_;
    const auto count = static_cast<std::uint32_t>(host_.manifest().size());
    if (controls_.paramCount != count) {
        controls_.paramCount = count;
        changed = true;
    }

    const auto learning = host_.learning();
    if (controls_.learning != learning || controls_.appliedEdit != lastPatchEdit_) {
        controls_.learning = learning;
        controls_.appliedEdit = lastPatchEdit_;
        changed = true;
    }

    /*  The parameters a remote window draws are read live from the host, not from the editing copy:
     *  a knob the user is turning has to arrive at the other window as it turns. */
    for (int i = 0; i < static_cast<int>(count) && i < static_cast<int>(kMaxParams); ++i) {
        const float value = host_.parameterValue(i);
        if (controls_.params[static_cast<std::size_t>(i)] != value) {
            controls_.params[static_cast<std::size_t>(i)] = value;
            changed = true;
        }
    }

    //  The sequence is read before the patch is packed, so a commit landing in between only packs again.
    const std::uint64_t document = host_.document().nextSequence();
    const bool repack = document != controlsDocument_ || !controlsPublished_;
    const bool moved = host_.document().withEditing([&](const PluginState& s, const UndoStack& u) {
        bool any = false;
        const auto canUndo = static_cast<std::uint8_t>(u.canUndo() ? 1 : 0);
        const auto canRedo = static_cast<std::uint8_t>(u.canRedo() ? 1 : 0);
        if (controls_.canUndo != canUndo || controls_.canRedo != canRedo) {
            controls_.canUndo = canUndo;
            controls_.canRedo = canRedo;
            any = true;
        }
        //  compared as the bus holds them, cut to its field, so a long name does not republish every tick
        const auto fits = [](std::string_view s) { return s.substr(0, kLinkEditNameBytes); };
        if (controls_.undoNameString() != fits(u.undoName()) || controls_.redoNameString() != fits(u.redoName())) {
            controls_.setUndoNames(u.undoName(), u.redoName());
            any = true;
        }
        if (repack) {
            packPatch(s, controls_.patch);
            any = true;
        }
        return any;
    });
    if (repack) controlsDocument_ = document;
    changed = changed || moved;

    if (changed) {
        bus_.publishControls(controls_);
        controlsPublished_ = true;
    }
}

void LinkNode::applyPatchEdit() {
    //  Validated before it can become an undo step: it came from another process.
    PluginState probe{host_.manifest()};
    if (unpackPatch(host_.modManifest(), incoming_.patch, probe)) {
        const auto name = incoming_.nameString();
        const auto key = incoming_.keyString();
        const auto& mod = host_.modManifest();
        const auto& edit = incoming_;
        const auto change = [&mod, &edit](PluginState& s) { unpackPatch(mod, edit.patch, s); };
        if (key.empty())
            host_.document().edit(name.empty() ? "remote edit" : name, change);
        else
            //  one window's drag, never another's
            host_.document().editCoalescing(name, incoming_.from.toString() + "." + key, change);
        ++status_.remoteEdits;
    }
    if ((incoming_.flags & kLinkEditEnd) != 0) host_.document().endGesture();
}

LinkStatic LinkNode::buildStatic() const {
    LinkStatic st;
    const Identity id = host_.identity();
    //  the user's name for it, or the track's
    if (!id.label.empty())
        st.setLabel(id.label);
    else {
        const std::lock_guard<std::mutex> lock(trackMutex_);
        st.setLabel(trackName_.toStdString());
    }
    st.colour = static_cast<std::uint8_t>(std::clamp(id.colour, 0, kNumColours - 1));
    st.order = static_cast<std::uint8_t>(std::clamp(host_.order(), 0, 255));
    host_.describe(st);  // the encoder's path and centre; an effect has none
    return st;
}

void LinkNode::updateStatus() {
    const Identity id = host_.identity();
    status_.open = bus_.isOpen();
    status_.session = id.session.isNil() ? juce::String() : juce::String(id.session.toString());
    status_.instance = id.instance.isNil() ? juce::String() : juce::String(id.instance.toString());
    status_.slot = bus_.selfSlot();
    status_.peers = status_.open ? static_cast<int>(peers_.size()) : 0;
    status_.rejoins = bus_.rejoins();
    status_.error = juce::String(bus_.error().empty() ? directory_.error() : bus_.error());
    directory_.list(sessions_, linkNowMicros());
    status_.liveSessions = static_cast<int>(sessions_.size());
}

bool LinkNode::readControls(const LinkScene::Entry& peer, LinkControls& out, std::uint32_t& generation) const {
    return bus_.readControls(peer.peer(), out, generation);
}

bool LinkNode::sendParameter(const Uuid& target, ParamId id, float value, std::uint32_t gesture) {
    LinkCommand command;
    command.param = static_cast<std::uint32_t>(id);
    command.value = value;
    command.gesture = gesture;
    return bus_.sendCommand(target, command);
}

std::uint32_t LinkNode::sendPatchEdit(const Uuid& target, const LinkPatch& patch, std::string_view name,
                                      std::string_view key, bool end) {
    outgoing_.patch = patch;
    outgoing_.setName(name);
    outgoing_.setKey(key);
    outgoing_.flags = end ? kLinkEditEnd : 0;
    return bus_.sendPatchEdit(target, outgoing_);
}

bool LinkNode::sendCommand(const Uuid& target, std::uint32_t command, float value) {
    LinkCommand c;
    c.param = command;
    c.value = value;
    //  Sent as a gesture endpoint, so a full inbox still takes it: its last slots are kept for these.
    c.gesture = kLinkGestureEnd;
    return bus_.sendCommand(target, c);
}

}  // namespace bambi::host
