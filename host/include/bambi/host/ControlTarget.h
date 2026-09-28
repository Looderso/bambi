// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

#include "bambi/host/LinkNode.h"
#include "bambi/link/link.hpp"
#include "bambi/patch/undo.hpp"

/*  The instance the controls show and edit: this one, or another in the session. Shared by every
 *  plugin, built on `LinkHost` and `LinkNode`, which all three have.
 *
 *  It lives in `host` and not in `ui` because it is about a Document, a LinkNode and host
 *  parameters, and `ui` does not depend on `host`. What the window sees is `ui::PatchModel`; a
 *  plugin's control state is what joins the two.
 */
namespace bambi::host {

/// The little a target needs that `LinkHost` and `LinkNode` do not already answer. Everything with a default is something a plugin may simply not have.
class TargetHost {
public:
    virtual ~TargetHost() = default;

    virtual LinkHost& linkHost() = 0;
    virtual LinkNode& linkNode() = 0;

    /// A modulation source's value as the engine has it now, after its amount.
    virtual float liveSourceValue(int slot) const = 0;
    /// What the rates have turned a region slot by, in radians -- axis 0 yaw, 1 pitch, 2 roll -- as the audio thread last published it. Pairs with `zeroRegionTurn`.
    virtual float regionTurn(int /*slot*/, int /*axis*/) const noexcept { return 0.0f; }
    /// Which envelope is armed to learn, or -1. Arming, and clearing a turn, are `LinkHost`'s.
    virtual int noteLearnArmed() const { return -1; }
};

/*  The instance the controls show and edit: this one, or another in the session, selected in the
    scene. The matrix, the tabs and a source's settings see only this, so every control works the
    same on either.

    Parameter values are normalised, 0 to 1, as a host parameter's are: the manifest's ranges are linear. */
class ControlTarget {
public:
    virtual ~ControlTarget() = default;

    /// False while another instance's controls have not arrived yet.
    virtual bool ready() const = 0;
    /// The patch and the parameter values, as the controls should show them.
    virtual PluginState state() const = 0;

    virtual void edit(std::string_view name, const UndoStack::Edit& change) = 0;
    virtual void editDrag(std::string_view name, std::string_view key, const UndoStack::Edit& change) = 0;
    virtual void endDrag() = 0;

    virtual void beginParameter(ParamId id) = 0;
    virtual void setParameter(ParamId id, float normalised) = 0;
    virtual void endParameter(ParamId id) = 0;
    /// One change, bracketed as a gesture.
    void changeParameter(ParamId id, float normalised);

    virtual bool canUndo() const = 0;
    virtual bool canRedo() const = 0;
    virtual std::string undoName() const = 0;  ///< what undo would revert; empty when nothing
    virtual std::string redoName() const = 0;
    virtual void undo() = 0;
    virtual void redo() = 0;

    virtual float sourceValue(int slot) const = 0;  ///< live, after its amount
    virtual int learning() const = 0;               ///< the envelope waiting for a note, or -1
    virtual void learn(int envelope) = 0;           ///< -1 stops
    /// Clear a rate's accumulated turn -- 0 yaw, 1 pitch, 2 roll. The placement's is the encoder's alone; a region's belongs to a slot, and every plugin has at least one.
    virtual void zeroTurn(int) {}
    virtual void zeroRegionTurn(int, int) {}
};

/// This instance: its own processor, host parameters and undo stack.
class LocalTarget final : public ControlTarget {
public:
    explicit LocalTarget(TargetHost& host) : host_(host) {}

    bool ready() const override { return true; }
    PluginState state() const override;
    void edit(std::string_view name, const UndoStack::Edit& change) override;
    void editDrag(std::string_view name, std::string_view key, const UndoStack::Edit& change) override;
    void endDrag() override;
    void beginParameter(ParamId id) override;
    void setParameter(ParamId id, float normalised) override;
    void endParameter(ParamId id) override;
    bool canUndo() const override;
    bool canRedo() const override;
    std::string undoName() const override;
    std::string redoName() const override;
    void undo() override;
    void redo() override;
    float sourceValue(int slot) const override;
    int learning() const override;
    void learn(int envelope) override;
    void zeroTurn(int axis) override;
    void zeroRegionTurn(int slot, int axis) override;

private:
    TargetHost& host_;
};

/*  Another instance, through the link bus.

    What it shows is what that instance last published, copied only when the generation moves. An
    edit is made on a copy of that, sent whole, and shown at once: held as pending until the target
    reports it applied -- or for a second, if it never does. A parameter being dragged shows the
    value sent until the target's own value catches up. So the controls answer the mouse
    immediately, however far behind the other instance's timer is. */
class RemoteTarget final : public ControlTarget {
public:
    explicit RemoteTarget(TargetHost& host) : host_(host) {}
    ~RemoteTarget() override;

    /// Follow `peer`, once a frame of `dt` seconds. Moving to another instance ends whatever was still open on this one.
    void follow(const LinkScene::Entry& peer, double dt);
    /// End every gesture still open: the selection is moving away, or the window is closing.
    void release();
    const Uuid& instance() const { return instance_; }

    /// Its energy picture. While wanted, `follow` keeps asking the instance for its summary and reads each new one; `takeEnergy` hands over the newest once, at the order it was sent at.
    void wantEnergy(bool wanted) { energyWanted_ = wanted; }
    bool takeEnergy(std::vector<float>& added, std::vector<float>& arrived, int& order);

    bool ready() const override { return generation_ != 0; }
    PluginState state() const override;
    void edit(std::string_view name, const UndoStack::Edit& change) override;
    void editDrag(std::string_view name, std::string_view key, const UndoStack::Edit& change) override;
    void endDrag() override;
    void beginParameter(ParamId id) override;
    void setParameter(ParamId id, float normalised) override;
    void endParameter(ParamId id) override;
    bool canUndo() const override { return controls_.canUndo != 0; }
    bool canRedo() const override { return controls_.canRedo != 0; }
    std::string undoName() const override { return controls_.undoNameString(); }
    std::string redoName() const override { return controls_.redoNameString(); }
    void undo() override;
    void redo() override;
    float sourceValue(int slot) const override;
    int learning() const override { return controls_.learning; }
    void learn(int envelope) override;
    void zeroTurn(int axis) override;
    void zeroRegionTurn(int slot, int axis) override;

private:
    void send(std::string_view name, std::string_view key, const UndoStack::Edit& change);
    float plainValue(ParamId id) const;  ///< as shown now, in the parameter's own units

    struct Held {
        float plain{0.0f};
        double seconds{0.0};  ///< shown for this long without arriving
        bool active{false};   ///< shown instead of the target's value
        bool open{false};     ///< between begin and end
    };

    TargetHost& host_;
    Uuid instance_;
    LinkControls controls_{};
    LinkControls scratch_{};
    LinkPatch pending_{};
    std::uint32_t generation_{0};
    std::uint32_t pendingSequence_{0};
    double pendingSeconds_{0.0};
    bool hasPending_{false};
    bool dragging_{false};
    std::string dragName_, dragKey_;
    /// Sized by the cap every plugin shares, not by one plugin's count: this is the suite's target now, and `kNumEncoderParams` is the encoder's.
    std::array<Held, kMaxParams> held_{};
    std::array<float, kNumSources> sources_{};
    LinkEnergy energy_{}, scratchEnergy_{};
    std::uint32_t energyGeneration_{0}, energyTaken_{0};
    bool energyWanted_{false};
};

}  // namespace bambi::host
