// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "bambi/link/latest.hpp"
#include "bambi/math/vec3.hpp"
#include "bambi/mod/manifest.hpp"
#include "bambi/patch/identity.hpp"
#include "bambi/patch/parameters.hpp"
#include "bambi/patch/state.hpp"
#include "bambi/path/trajectory.hpp"

namespace bambi {

/*  The link bus -- how instances see each other without the host's help. Transport is POSIX shared memory keyed by
 *  the session's uuid, so two open projects never see each other.
 *
 *  Four kinds of data travel: STATIC (what an instance is, generation counted so a reader copies it only when it
 *  changed), DYNAMIC (where the source is now, published every frame), COMMAND (a parameter edit from another
 *  instance's window, applied through the target's own host parameter so it stays automatable and undoable), and
 *  CONTROLS (a window's parameter values and patch, and an edited patch written back from elsewhere). Nothing on
 *  the audio thread touches this memory: it hands its dynamic state to a LinkPublisher through a process-local
 *  LatestValue, and a message-thread timer publishes it.
 */

inline constexpr int kLinkMaxInstances = 64;
inline constexpr int kLinkLabelBytes = 32;
inline constexpr int kLinkInboxSlots = 32;  ///< commands buffered per instance
/*  A path on the bus: a point every kLinkPathStepRad of its length, between kLinkMinPathPoints and kLinkPathPoints
 *  of them, sized for the max (~49 KB) and copied only when it changes. */
inline constexpr int kLinkPathPoints = 4096;  ///< the most a path is sent with, and the array's size
inline constexpr int kLinkMinPathPoints = 128;
inline constexpr double kLinkPathStepRad = 1.0 * kDeg2Rad;
int linkPathPointsFor(double lengthRad);
inline constexpr int kLinkInboxReserved = 4;  ///< inbox slots only gesture begin/end may use
/// Bump whenever the shared layout changes, so no instance ever maps a bus laid out another way.
inline constexpr std::uint32_t kLinkVersion = 22;

//  Which plugin an instance is: `patch/identity.hpp`, because the state format needs it too.

/// How long an instance may go silent before its slot is reclaimed.
inline constexpr std::uint64_t kLinkHeartbeatTimeoutUs = 2'000'000;

//  Controlling another instance.
///  The bus and every engine size their parameter arrays by kMaxParams (patch/parameters.hpp); the Segment layout
///  is static_asserted below, so a mismatch is a compile error rather than a bus that silently never joins.
inline constexpr int kLinkMaxParams = kMaxParams;
/*  Room for sources, not the number of them: a multiple of eight, so that neither array of them ends
 *  a struct short of its alignment, and so that the next source is not another change of layout. */
inline constexpr int kLinkMaxSources = 24;
inline constexpr int kLinkMaxCells = 855;  ///< one cell per (source, target): 19 sources x the encoder's 45 targets
inline constexpr int kLinkTriggers = 3;
inline constexpr int kLinkEditNameBytes = 32;

/// An edit lock older than this was left by a sender that died holding it.
inline constexpr std::uint64_t kLinkEditLockStaleUs = 100'000;

inline constexpr std::uint32_t kLinkEditEnd = 1;  ///< LinkPatchEdit::flags: the sender's drag has ended

/// Commands that are not parameter edits. `param` holds one of these, outside every ParamId.
inline constexpr std::uint32_t kLinkCommandUndo = 0x10000;
inline constexpr std::uint32_t kLinkCommandRedo = 0x10001;
inline constexpr std::uint32_t kLinkCommandLearn = 0x10002;  ///< value: the envelope to learn a note for; -1 stops
inline constexpr std::uint32_t kLinkCommandZeroTurn =
    0x10003;  ///< value: the axis whose integrated turn is cleared, 0 yaw 1 pitch 2 roll
/*  The same for a region's turn: value is slot * 3 + axis. No version bump needed -- an older
 *  build ignores a command it doesn't recognise (`applyCommand`'s default arm). */
inline constexpr std::uint32_t kLinkCommandZeroRegionTurn = 0x10004;
inline constexpr int kZeroRegionTurnAxes = 3;  ///< how the slot and the axis are packed into one value

struct LinkPoint {
    float x{0.0f}, y{0.0f}, z{0.0f};
};

/*  STATIC section: what an instance is. POD, fixed width, no pointers, shared between processes that may be
 *  different builds. The path travels as a polyline, not a generator descriptor: draw-ready and version-proof, so
 *  no receiver has to rebuild an arc-length table or depend on generator code. */
struct LinkStatic {
    char label[kLinkLabelBytes]{};  ///< NUL-padded, not necessarily terminated
    std::uint8_t colour{0};
    std::uint8_t order{1};
    std::uint8_t closed{1};
    std::uint8_t reserved{0};
    std::uint32_t pointCount{0};         ///< 0 until setPath; linkPathPointsFor(its length) after
    LinkPoint centre{0.0f, 0.0f, 1.0f};  ///< what the transform rotates and shrinks about
    LinkPoint path[kLinkPathPoints]{};   ///< untransformed; apply LinkDynamic::transform()

    std::string labelString() const;
    void setLabel(std::string_view s);

    /*  Samples `t` at equal arc-length steps, linkPathPointsFor(its length) of them: a closed
     *  path over [0, 1) since the loop is implied, an open one over [0, 1] so both ends are present. */
    void setPath(const Trajectory& t);

    Vec3 point(int i) const;
    Vec3 centreVec() const;
};

/*  An instance's energy summary, for another window's picture: the upper triangle of each channel covariance, cut
 *  to at most order 3. Display only; published only while `askForEnergy` has been called recently, since gathering
 *  it costs. */
inline constexpr int kLinkEnergyOrder = 3;
inline constexpr int kLinkEnergyValues = 136;  ///< covarianceSize(kLinkEnergyOrder): (16 * 17) / 2
struct LinkEnergy {
    std::uint8_t order{0};        ///< what the summary is at, 0..kLinkEnergyOrder
    std::uint8_t withArrived{0};  ///< an effect's: an encoder has nothing arriving
    std::uint16_t reserved{0};
    std::uint32_t count{0};  ///< numbers in each triangle: covarianceSize(order)
    float added[kLinkEnergyValues]{};
    float arrived[kLinkEnergyValues]{};
};

/*  A region as its owner's audio thread resolved it, for another instance's scene to draw.
 *  Display only -- no side or amount, since nothing is heard from it -- and floats, as the
 *  positions are. Weights matter only for clouds and custom, and cost nothing for the rest. */
struct LinkRegionShape {
    std::uint8_t kind{0};  ///< RegionKind
    std::uint8_t sectors{4};
    std::uint8_t dots{6};
    std::uint8_t reserved{0};
    std::int32_t seed{1};
    float yaw{0.0f}, pitch{0.0f}, roll{0.0f}, softness{0.0f};
    float size{0.0f}, bandElevation{0.0f}, thickness{0.0f}, fill{0.0f}, dotSize{0.0f};
    float coverage{0.0f}, contrast{0.0f}, detail{0.0f}, evolve{0.0f};
    float weights[kRegionWeights]{};

    void set(const Region& r);
    Region region() const;  ///< the side is Inside: an edge is the same either way
};

/*  DYNAMIC section: where the source is now. Published at scene rate. */
struct LinkDynamic {
    float s{0.0f};                    ///< normalised arc length, after the movement mode
    float x{1.0f}, y{0.0f}, z{0.0f};  ///< resolved position: transform applied
    float level{0.0f};                ///< for the scene's activity indication
    float yawRad{0.0f}, pitchRad{0.0f}, rollRad{0.0f}, extent{1.0f};
    float sources
        [kLinkMaxSources]{};  ///< the first kNumSources: every modulation source after its amount, for a source's settings
    std::uint8_t muted{0};
    /// What a stereo input puts on the sphere: 0 sum, 1 mid/side, 2 stereo (`InputMode`). In `sum`, the two points
    /// below are the position.
    std::uint8_t inputMode{0};
    std::uint8_t reserved[2]{};
    std::uint32_t reserved2{0};
    float leftX{1.0f}, leftY{0.0f}, leftZ{0.0f};     ///< the input's left point, resolved as the position is
    float rightX{1.0f}, rightY{0.0f}, rightZ{0.0f};  ///< and its right
    std::uint32_t reserved3{0};                      ///< keeps the 64-bit stamps below on their own boundary
    std::uint64_t sampledUs{0};                      ///< when the audio thread computed this (linkNowMicros)
    std::uint64_t publishedUs{0};                    ///< when it was published: set by LinkPublisher::tick
    /// Every region slot the owner has, as resolved this step: 1 for the encoder and Echo, 2 for Reverb.
    std::uint32_t regionCount{0};
    std::uint32_t reserved4{0};
    LinkRegionShape regions[kMaxRegions]{};
    /// Every parameter as the engine has it this step -- modulation, automation and smoothing included.
    std::uint32_t liveCount{0};
    float live[kMaxParams]{};

    /// Carried rather than recomputed from path and `s`: the sender's ground truth, letting a scene skip resolving
    /// paths it isn't drawing.
    Vec3 position() const { return {x, y, z}; }
    Vec3 left() const { return {leftX, leftY, leftZ}; }
    Vec3 right() const { return {rightX, rightY, rightZ}; }
    PathTransform transform() const;
};

/// One live instance, as seen by poll().
struct LinkPeer {
    Uuid instance{};
    Product product{Product::Unknown};  ///< what it is; the scene draws every kind
    int slot{-1};
    std::uint32_t staticGeneration{0};    ///< 0 until the peer first publishes its static section
    std::uint32_t controlsGeneration{0};  ///< 0 until the peer first publishes its controls
    LinkDynamic dyn{};

    /*  False until the peer has published a dynamic section. A slot is visible as soon as its
     *  instance joins, since its path may already be here, but until the first publish `dyn` is
     *  join()'s default -- not a real position, and a scene must not draw a dot for it. */
    bool hasPosition{false};
};

inline constexpr std::uint32_t kLinkGestureNone = 0;
inline constexpr std::uint32_t kLinkGestureBegin = 1;
inline constexpr std::uint32_t kLinkGestureEnd = 2;

/*  A remote parameter edit, applied on the target's message thread through its own host parameter (see
 *  LinkEditReceiver). Begin and end carry a value -- press and release -- so the parameter lands correctly even if
 *  values between are dropped by a full inbox; an edit that ends for any reason sends an end. */
struct LinkCommand {
    Uuid from{};
    std::uint32_t param{};  ///< ParamId as an integer; validated on receipt
    float value{0.0f};
    std::uint32_t gesture{kLinkGestureNone};

    ParamId paramId() const;
};

/*  A patch on the bus: everything in a PluginState that is neither a host parameter nor identity -- trajectory,
 *  matrix, envelope triggers, source slots. Fixed width, no compiler padding, so doubles stay doubles and a round
 *  trip through another window is bit-identical. */
struct LinkNode {
    double p[3]{1.0, 0.0, 0.0};
    double cin[3]{1.0, 0.0, 0.0};
    double cout[3]{1.0, 0.0, 0.0};
    std::uint8_t smooth{1};
    std::uint8_t reserved[7]{};
};

struct LinkCell {
    std::uint8_t tab{0};
    std::uint8_t source{0};
    std::uint16_t target{0};
    std::uint32_t reserved{0};
    double depth{0.0};
};

struct LinkTrigger {
    std::uint8_t input{0}, gate{0}, noteLow{36}, noteHigh{36}, channel{0};
    std::uint8_t reserved[3]{};
    std::int32_t source{1};
    std::uint32_t reserved2{0};
    double velocity{0.0}, threshold{0.35}, hysteresis{0.10};
};

struct LinkSourceSlot {
    std::uint8_t input{0};
    std::uint8_t reserved[3]{};
    Uuid link{};
};

/*  A region slot, as a patch carries it: kind and counts only, since angles, sizes and which side
 *  passes are host parameters. The rest of what an effect's patch has beyond the trajectory. */
struct LinkRegion {
    std::uint8_t kind{0};
    std::uint8_t sectors{4};
    std::uint8_t dots{6};
    std::uint8_t reserved{0};
};

struct LinkPatch {
    std::uint8_t kind{0}, closed{1}, generator{0}, nodeCount{0};
    std::uint32_t cellCount{0};
    /*  What an effect's patch is, where the trajectory is what the encoder's is; carried by every product, since
     *  one patch type is what lets one bus carry all three. */
    LinkRegion regions[kMaxRegions]{};
    std::uint8_t renderQuality{1};  ///< RenderQuality; 1 = Realistic, the default
    std::uint8_t reservedPatch[3]{};
    double genParams[kMaxGenParams]{};
    LinkNode nodes[kMaxNodes]{};
    LinkCell cells[kLinkMaxCells]{};
    LinkTrigger triggers[kLinkTriggers]{};
    LinkSourceSlot sources[kLinkMaxSources]{};  ///< the first kNumSources
};

/// A state's patch. Cells without depth are left out; more than kLinkMaxCells, which no valid state has, are cut.
void packPatch(const PluginState& s, LinkPatch& out);

/*  A patch into `s`, leaving its host parameters and identity alone. Validated as untrusted input: an out-of-range
 *  count or a non-finite number refuses the whole patch; what can be repaired is (enums clamp, a bad cell drops). */
bool unpackPatch(const ModManifest& m, const LinkPatch& p, PluginState& s);

/*  Controls: what a window controlling this instance shows -- parameter values and patch. Written by the owner,
 *  generation-counted, published when something changes. */
struct LinkControls {
    std::uint32_t paramCount{0};     ///< how many of `params` the writer has
    float params[kLinkMaxParams]{};  ///< plain values, by ParamId
    std::uint8_t canUndo{0}, canRedo{0};
    std::int8_t learning{-1};  ///< the envelope waiting for a note, or -1
    std::uint8_t reserved{0};
    std::uint32_t appliedEdit{0};  ///< the last LinkPatchEdit::sequence applied
    std::uint32_t reserved2{0};
    char undoName[kLinkEditNameBytes]{};  ///< what undo would revert, NUL-padded; empty when nothing
    char redoName[kLinkEditNameBytes]{};
    LinkPatch patch{};

    void setUndoNames(std::string_view undo, std::string_view redo);
    std::string undoNameString() const;
    std::string redoNameString() const;
};

/*  An edit from another window: the whole patch after the edit, not the edit itself. Latest wins:
 *  nothing is lost when a newer one overwrites an older one, since it already contains it. */
struct LinkPatchEdit {
    Uuid from{};                      ///< stamped by sendPatchEdit
    std::uint32_t sequence{0};        ///< stamped by sendPatchEdit; 0 = none
    std::uint32_t flags{0};           ///< kLinkEditEnd
    char name[kLinkEditNameBytes]{};  ///< the undo step's name, NUL-padded
    char key[kLinkEditNameBytes]{};   ///< a drag's coalescing key; empty for a single edit
    LinkPatch patch{};

    void setName(std::string_view s);
    void setKey(std::string_view s);
    std::string nameString() const;
    std::string keyString() const;
};

/*  One instance's handle on the session's bus. Every method is for the message thread; the audio thread uses only
 *  LinkPublisher::submit. Destruction releases the slot at once rather than waiting for the heartbeat to lapse. */
class LinkBus {
public:
    LinkBus() = default;
    ~LinkBus();
    LinkBus(const LinkBus&) = delete;
    LinkBus& operator=(const LinkBus&) = delete;

    /*  Join `session` as `instance`, creating the segment if this is the first instance. False and error() set if
     *  shared memory is unavailable; every other method is then a safe no-op. `product` is not defaulted, since a
     *  plugin that forgot to say would silently become an encoder to everything else on the bus. */
    bool open(const Uuid& session, const Uuid& instance, Product product);
    void close();
    bool isOpen() const { return map_ != nullptr; }
    const std::string& error() const { return error_; }

    /// Publish what this instance IS. Bumps the static generation. Call on edit, not per frame.
    void publishStatic(const LinkStatic& st);

    /// Publish where the source is now, and stamp the heartbeat.
    void publishDynamic(const LinkDynamic& d);

    /// Every live instance's dynamic section, this instance included. Allocates.
    void poll(std::vector<LinkPeer>& out) const;

    /*  Copies a peer's static section, with `generation` read under the same lock. False -- `out` unspecified -- if
     *  the slot changed owner, the peer never published one, or a concurrent write outlasted the retry bound. */
    bool readStatic(const LinkPeer& peer, LinkStatic& out, std::uint32_t& generation) const;

    /// Publish this instance's controls. Bumps their generation: call when they change.
    void publishControls(const LinkControls& c);

    /// A peer's controls, with their generation. The contract is readStatic's.
    bool readControls(const LinkPeer& peer, LinkControls& out, std::uint32_t& generation) const;

    /// The energy summary, published while it has been asked for and read by generation, as the controls are.
    void publishEnergy(const LinkEnergy& e);
    bool readEnergy(const LinkPeer& peer, LinkEnergy& out, std::uint32_t& generation) const;
    /// Ask a peer for its energy until `untilUs` (linkNowMicros). False if it is gone.
    bool askForEnergy(const LinkPeer& peer, std::uint64_t untilUs);
    /// Until when someone has asked for this instance's energy; 0 when nobody has.
    std::uint64_t energyAskedUntil() const;

    /*  Queues a parameter edit on another instance. False if that instance is gone or its inbox is full. The last
     *  kLinkInboxReserved slots are reserved for gesture begin/end, since a stream of values fills the rest first,
     *  harmlessly, while the endpoints still fit. */
    bool sendCommand(const Uuid& target, const LinkCommand& c);

    /// Take everything addressed to this instance.
    void drainCommands(std::vector<LinkCommand>& out);

    /*  Writes a whole patch into another instance's edit cell: the stamped sequence, or 0 if gone or locked past the
     *  retry bound. The cell is a seqlock, so writers take a CAS'd timestamp lock first, stale after kLinkEditLockStaleUs. */
    std::uint32_t sendPatchEdit(const Uuid& target, const LinkPatchEdit& edit);

    /// The newest edit addressed to this instance, if its sequence is not `lastSequence` -- which it becomes.
    bool takePatchEdit(LinkPatchEdit& out, std::uint32_t& lastSequence);

    Product product() const { return product_; }  ///< what this instance told the bus it is

    int slotCount() const;  ///< live instances, for tests and diagnostics
    int selfSlot() const { return slot_; }

    /// How many times this bus found its slot taken and rejoined (see rejoin()): nonzero means the process was
    /// suspended long enough to be reaped and resumed.
    int rejoins() const { return rejoins_; }

    /*  True when open(), or a rejoin, stepped back because another live instance already uses this id -- what a
     *  duplicated or pasted instance carries. The owner must then take a fresh id and open again, or two slots
     *  would answer to one. */
    bool instanceConflict() const { return instanceConflict_; }

    /*  The shared-memory object a session's bus lives in. The layout version is part of the name, so a build whose
     *  slots differ never maps another build's object -- macOS sizes a POSIX object only once, so meeting a
     *  smaller one is unrecoverable until reboot. */
    static std::string segmentNameFor(const Uuid& session);

    /// Remove the session's segment from the system. Tests only; not for plugin use.
    static void unlinkSession(const Uuid& session);

private:
    int reapDead();  ///< reclaims slots whose heartbeat has lapsed
    bool join(const Uuid& session, const Uuid& instance, Product product);
    bool ownsSlot() const;
    bool rejoin();
    void writeStatic(const LinkStatic& st);
    void writeControls(const LinkControls& c);

    void* map_{nullptr};
    std::size_t mapBytes_{0};
    std::intptr_t handle_{-1};  ///< the OS's handle on the segment
    int slot_{-1};
    int rejoins_{0};
    Uuid session_{}, instance_{};
    Product product_{Product::Unknown};
    std::string name_, error_;
    LinkStatic lastStatic_{};  ///< republished after a rejoin
    bool hasStatic_{false};
    LinkControls lastControls_{};  ///< republished after a rejoin
    bool hasControls_{false};
    bool instanceConflict_{false};
};

/*  The bridge from the audio thread to the bus. submit() is the only thing the audio thread calls: wait-free,
 *  process-local. tick() runs on a message-thread timer at scene rate and does the publishing. */
class LinkPublisher {
public:
    explicit LinkPublisher(LinkBus& bus) : bus_(bus) {}

    /// Audio thread.
    void submit(const LinkDynamic& d) noexcept { latest_.write(d); }

    /*  Message thread. What tick() publishes before audio starts, so a freshly loaded instance doesn't appear at
     *  the front of the sphere before its first block runs. */
    void seed(const LinkDynamic& d) { last_ = d; }

    /*  Message thread, from a timer. Republishes the last state when nothing new arrived -- a host that stops
     *  calling the audio callback (transport stopped, track frozen, bypassed) stops submit() too, and liveness
     *  belongs to the plugin, not to its audio. */
    void tick();

private:
    LinkBus& bus_;
    LatestValue<LinkDynamic> latest_;
    LinkDynamic last_{};
};

/*  The reader side: a scene's cached view of every instance. update() copies every peer's dynamic section, but a
 *  static section only when its generation has moved, since 64 paths run to about 3 MB. UI thread; allocates. */
class LinkScene {
public:
    struct Entry {
        Uuid instance{};
        LinkDynamic dyn{};
        LinkStatic st{};
        std::uint32_t generation{0};  ///< of `st`; 0 = not received yet
        /*  What this peer is. A scene draws every kind, but only an encoder has a source -- an effect's dynamic
         *  section carries modulation sources and no position, so a dot drawn for one would mean nothing. */
        Product product{Product::Unknown};
        bool hasPosition{false};  ///< see LinkPeer::hasPosition; it has published, no more
        int slot{-1};             ///< where the peer is, for readControls
        std::uint32_t controlsGeneration{0};

        LinkPeer peer() const;  ///< this entry as LinkBus reads it
        /// What a window calls it: its published label, or "instance" and the first four of its id.
        std::string label() const;
    };

    void update(const LinkBus& bus);

    const std::vector<Entry>& entries() const { return entries_; }
    const Entry* find(const Uuid& instance) const;

    /// Static sections copied so far. Diagnostics -- and the proof the cache works.
    std::uint64_t staticReads() const { return staticReads_; }

private:
    std::vector<Entry> entries_;
    std::vector<LinkPeer> peers_;
    std::vector<std::uint8_t> seen_;
    LinkStatic scratch_{};
    std::uint64_t staticReads_{0};
};

/// One thing a remote edit asks the host to do, mapped one to one onto beginChangeGesture, setValueNotifyingHost
/// and endChangeGesture.
struct LinkEditAction {
    enum class Kind : std::uint8_t { Begin, Value, End };
    Kind kind{Kind::Value};
    ParamId param{kNoParamId};
    float value{0.0f};      ///< for Value
    bool abandoned{false};  ///< an End issued because the sender is gone, not because it said so
};

/*  The receiving side of remote edits, and the owner of their gestures.
 *
 *  A host gesture that begins must end, or the host believes an edit is still in progress and refuses other
 *  changes to that parameter. The sender cannot guarantee this -- an end may be refused by a full inbox, or never
 *  sent if the sender crashes -- so the lifecycle is enforced here: every Value falls between a Begin and an End,
 *  gestures never nest, begin and end always carry a value so the parameter lands where the user let go, and last
 *  begin wins so a stalled window can never lock a parameter for everyone. A value or end with no open gesture is
 *  applied as a complete Begin/Value/End. closeAbandoned() ends gestures whose sender is no longer alive. No idle
 *  timeout: a held control sends nothing, and timing it out would drop touch automation back to the old curve.
 *
 *  Keyed by parameter. Message thread. Both methods append to `out`.
 */
class LinkEditReceiver {
public:
    void apply(std::span<const LinkCommand> commands, std::vector<LinkEditAction>& out);
    void closeAbandoned(std::span<const LinkPeer> live, std::vector<LinkEditAction>& out);

    bool isOpen(ParamId id) const;
    std::size_t openGestures() const;

private:
    struct Gesture {
        Uuid owner{};
        bool open{false};
    };
    std::array<Gesture, static_cast<std::size_t>(kMaxParams)> gestures_{};
};

/// A path as a scene draws it: untransformed points with the placement transform applied about the path's centre.
/// Allocates only when `out` has to grow.
void resolvePath(const LinkStatic& st, const LinkDynamic& dyn, std::vector<Vec3>& out);

/// Microseconds from a clock that is monotonic and comparable across processes.
std::uint64_t linkNowMicros();

/*  Is a heartbeat still fresh as of `now`? `now` is sampled once before the walk, so a peer publishing during it
 *  can leave hb > now; treating that as stale would underflow `now - hb` to about 1.8e19 and drop a live source, so
 *  a heartbeat in the future counts as fresh. */
bool linkHeartbeatAlive(std::uint64_t heartbeat, std::uint64_t now);

}  // namespace bambi
