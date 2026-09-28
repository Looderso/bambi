// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <functional>
#include <juce_audio_processors/juce_audio_processors.h>
#include <mutex>
#include <string>
#include <vector>

#include "bambi/host/Document.h"
#include "bambi/link/directory.hpp"
#include "bambi/link/link.hpp"
#include "bambi/mod/manifest.hpp"
#include "bambi/patch/identity.hpp"

/*  This instance on the link bus.
 *
 *  Joining a session, keeping an id across a save and a duplicate, publishing what this instance is
 *  and what it is doing, and taking in another window's edits -- none of that is one plugin's. The
 *  bus carries a product tag so all three can be on it; this is the other half of that, the side
 *  that lives in a plugin.
 *
 *  Everything here runs on the message thread, driven by a timer at kLinkTickHz. Nothing here is
 *  real-time, and nothing here is touched by an audio callback.
 */
namespace bambi::host {

/// Which directory the suite's instances find each other through. One name for all three plugins, and settable so a test can run a whole session of its own without touching a real one.
std::string& linkDirectoryName();
void setLinkDirectoryNameForTests(std::string name);

/// What the readout shows about the bus.
struct LinkStatus {
    bool open{false};
    juce::String session, instance, error;
    int slot{-1};
    int peers{0};
    int rejoins{0};
    int liveSessions{0};
    int remoteEdits{0};
};

/// What a LinkNode asks of the plugin it belongs to. Every one of these is called on the message thread, so a plain virtual is the right cost.
class LinkHost {
public:
    virtual ~LinkHost() = default;

    virtual Product product() const = 0;
    virtual const ParamManifest& manifest() const = 0;
    virtual const ModManifest& modManifest() const = 0;
    virtual Document& document() = 0;

    /// What this instance is, beyond the label, colour and order the node fills in: the encoder adds its path and centre, and an effect has none to add.
    virtual void describe(LinkStatic&) const {}

    /// Where this instance's dynamic section starts before any audio has run, so a stopped transport does not leave a scene drawing a default. An effect seeds nothing.
    virtual void seedDynamic(LinkDynamic&) const {}

    /// This plugin's host parameter for an id, or nullptr: how a remote edit is applied.
    virtual juce::RangedAudioParameter* hostParameter(ParamId) const = 0;

    /*  A parameter as the host has it now, read live. Not the editing copy's: that carries the
     *  values a patch was saved with, and a remote window must show the knob where it is this
     *  instant, not where the document last recorded it. */
    virtual float parameterValue(int at) const = 0;

    /// A command this build knows beyond those the node applies itself: undo, redo, a region's turn.
    virtual void applyCommand(const LinkCommand&) {}
    /// Clear a region slot's accumulated turn, one axis. Every plugin has a region, so the node unpacks the command and a plugin only says what clearing means to its engine.
    virtual void zeroRegionTurn(int /*slot*/, int /*axis*/) {}
    /// Arm an envelope to learn the next note, or -1 to stop; and clear the placement's turn, one axis, which is the encoder's alone.
    virtual void armNoteLearn(int /*envelope*/) {}
    virtual void zeroPlacementTurn(int /*axis*/) {}

    /// Something this instance publishes about itself that only it knows: the learn arming.
    virtual std::int8_t learning() const { return -1; }

    /// The identity label and colour, and what the host calls the track.
    virtual Identity identity() const = 0;
    /// The user's own name for this instance; empty gives it back to the track's. It is identity, not patch: it is saved with the state and is on no undo stack.
    virtual void renameInstance(const std::string& label) = 0;
    /*  The node has decided this instance's identity, and from now on it is the node's: a state
     *  loaded into a running instance -- a preset, the host's undo -- must not move it to another
     *  session or hand it another instance's id. Called on every join attempt, successful or not,
     *  because a failed join does not make the identity anyone else's either. */
    virtual void adoptIdentity(const Uuid& session, const Uuid& instance) = 0;

    /// The ambisonic order this instance is rendering at, for the scene's readout.
    virtual int order() const { return 1; }
};

class LinkNode {
public:
    /// Ticks a second. The scene's rate, and what every timing constant here is counted in.
    static constexpr int kTickHz = 60;

    explicit LinkNode(LinkHost& host) : host_(host), directory_(linkDirectoryName()) {}

    /// Closing. The last instance of a session to close unlists it, so a project started right afterwards does not join the one just closed. `slotCount()` counts every process, so this is the last instance anywhere and not merely the last in this one.
    ~LinkNode();

    /// Opening the directory. Failing is fine: a new instance then starts a session of its own. Separate from the constructor because a plugin wants it once it is otherwise built.
    void start() { directory_.open(); }

    /*  One tick. Joins if it is time to, publishes what has changed, and applies what has arrived.
     *  Joining is delayed a few ticks because a project that is loading sets this instance's state
     *  right after constructing it, and that state carries the session: joining at once would guess
     *  a session and then move. */
    void tick();

    /// Something this instance IS has changed: republish the static section on the next tick.
    void markStaticDirty() noexcept { staticDirty_ = true; }

    /*  The track's name, which labels this instance when the user has not named it themselves.
     *  Hosts may call `updateTrackProperties` on any thread, so it is held under a lock and read
     *  under one; the next tick republishes the static section with it.
     *
     *  It lives here and not in each plugin because it is the same member, the same mutex and the
     *  same dirty flag in all three. */
    void setTrackName(juce::String name) {
        {
            const std::lock_guard<std::mutex> lock(trackMutex_);
            trackName_ = std::move(name);
        }
        markStaticDirty();
    }

    /// Rename this instance, as its settings page does: through the host, which owns the identity, and republished at once so every window's tabs follow.
    void rename(const std::string& label) {
        host_.renameInstance(label);
        markStaticDirty();
    }
    /// What the host calls the track: the name an instance has until it is given one of its own.
    juce::String trackName() const {
        const std::lock_guard<std::mutex> lock(trackMutex_);
        return trackName_;
    }

    bool isOpen() const { return bus_.isOpen(); }
    const LinkStatus& status() const { return status_; }
    const LinkBus& bus() const { return bus_; }
    LinkDirectory& directory() { return directory_; }

    /// The user did something here: this is where they are working, and a new instance elsewhere should join this session rather than an older one.
    void noteActivity() {
        if (bus_.isOpen()) directory_.touch(host_.identity().session, linkNowMicros());
    }

    /// Where this instance is now, for every other instance's scene. Wait-free; the audio thread calls it.
    void publish(const LinkDynamic& d) { publisher_.submit(d); }

    void updateScene(LinkScene& scene) const { scene.update(bus_); }

    // ---- sending to another instance -------------------------------------------------------
    bool readControls(const LinkScene::Entry& peer, LinkControls& out, std::uint32_t& generation) const;
    bool sendParameter(const Uuid& target, ParamId id, float value, std::uint32_t gesture);
    std::uint32_t sendPatchEdit(const Uuid& target, const LinkPatch& patch, std::string_view name, std::string_view key,
                                bool end);
    bool sendCommand(const Uuid& target, std::uint32_t command, float value);

    // ---- another window's energy picture ---------------------------------------------------
    void publishEnergy(const LinkEnergy& e) { bus_.publishEnergy(e); }
    bool readEnergy(const LinkScene::Entry& peer, LinkEnergy& out, std::uint32_t& generation) const {
        return bus_.readEnergy(peer.peer(), out, generation);
    }
    bool askForEnergy(const LinkScene::Entry& peer, std::uint64_t untilUs) {
        return bus_.askForEnergy(peer.peer(), untilUs);
    }
    /// Someone elsewhere is looking at this instance's energy now.
    bool energyAskedFor() const { return bus_.energyAskedUntil() > linkNowMicros(); }

private:
    void join();
    void publishControls();
    void applyPatchEdit();
    LinkStatic buildStatic() const;
    void updateStatus();

    LinkHost& host_;

    //  declaration order matters: publisher_ holds a reference to bus_
    LinkDirectory directory_;
    LinkBus bus_;
    LinkPublisher publisher_{bus_};
    LinkEditReceiver receiver_;
    std::vector<LinkCommand> commands_;
    std::vector<LinkEditAction> actions_;
    std::vector<LinkPeer> peers_;
    std::vector<LinkSessionInfo> sessions_;
    LinkStatus status_;
    mutable std::mutex trackMutex_;
    juce::String trackName_;

    LinkControls controls_{};
    std::uint64_t controlsDocument_{0};
    bool controlsPublished_{false};
    LinkPatchEdit incoming_{}, outgoing_{};
    std::uint32_t lastPatchEdit_{0};

    bool staticDirty_{true};
    bool adopted_{false};
    int ticksBeforeFirstJoin_{kTickHz / 10};
    int ticksUntilRetry_{0};
};

}  // namespace bambi::host
