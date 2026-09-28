// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/link/link.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <thread>
#include <type_traits>

#include "bambi/mod/matrix.hpp"
#include "bambi/mod/sources.hpp"
#include "os.hpp"

namespace bambi {
namespace {

constexpr std::uint32_t kMagic = 0x41455448;  // 'AETH'

/*  One inbox entry. `ready` carries the absolute index of the command in it, plus one, and
 *  is published only after the payload is written.
 *
 *  Without it this ring is subtly broken, and the failure looks like duplicated commands
 *  rather than like a race: a producer that has CAS'd the head index but not yet stored its
 *  payload leaves the consumer free to read a cell that still holds the previous
 *  generation's contents. Claiming an index is not the same as publishing a value, and the
 *  consumer has to be able to tell those apart.
 */
struct InboxCell {
    std::atomic<std::uint32_t> ready;
    LinkCommand cmd;
};

/*  The shared layout. Everything is fixed-width and alignment is stated rather than assumed,
 *  because two processes mapping this may be different builds of different ages.
 *
 *  One slot per instance, owned by that instance: it is the only writer of everything in the
 *  slot except the inbox head. That single-writer property is what makes the seqlocks sound,
 *  and it is why the ownership check in publishDynamic exists.
 *
 *  The static and dynamic sections have separate seqlocks. A path is about 1.6 KB and a
 *  position about 50 bytes; under one lock, a reader that only wanted the position would
 *  have to race every path copy, and a path edit would stall every position read behind it.
 *
 *  std::atomic is used directly in shared memory, which is only sound because these are
 *  always lock-free for 32- and 64-bit integers on the platforms this ships to; the
 *  static_asserts below refuse to build anywhere that is not true, rather than silently
 *  placing a lock in memory that another process cannot see.
 */
struct Slot {
    std::atomic<std::uint32_t> occupied;   ///< claim flag, CAS'd
    std::atomic<std::uint32_t> dynSeq;     ///< even = stable, odd = being written
    std::atomic<std::uint64_t> heartbeat;  ///< linkNowMicros of the last dynamic publish
    std::atomic<std::uint32_t> staticSeq;
    std::atomic<std::uint32_t> staticGeneration;  ///< cheap to poll; may briefly lag the payload
    std::atomic<std::uint32_t> head;              ///< inbox: producers CAS this
    std::atomic<std::uint32_t> tail;              ///< inbox: owner only
    std::atomic<std::uint32_t> dropped;           ///< refused commands, for diagnosis
    std::atomic<std::uint32_t> controlsSeq;
    std::atomic<std::uint32_t> controlsGeneration;
    std::atomic<std::uint32_t> editSeq;       ///< the edit cell's seqlock: written only under editLock
    std::atomic<std::uint32_t> editSequence;  ///< the last stamped edit, cheap to poll
    std::atomic<std::uint64_t> editLock;      ///< 0 free; otherwise when a sender took it
    std::uint32_t staticGenerationPayload;    ///< written with `st`, under staticSeq
    std::uint32_t controlsGenerationPayload;  ///< written with `controls`, under controlsSeq
    Uuid instance;                            ///< written once in join(), before heartbeat
    std::uint32_t product;                    ///< Product, written with `instance` and as safely
    LinkDynamic dyn;
    LinkStatic st;
    LinkControls controls;
    LinkPatchEdit edit;
    InboxCell inbox[kLinkInboxSlots];
    std::atomic<std::uint64_t> energyAskedUs;  ///< a viewer's request: until when
    std::atomic<std::uint32_t> energySeq;
    std::atomic<std::uint32_t> energyGeneration;
    std::uint32_t energyGenerationPayload;  ///< written with `energy`, under energySeq
    std::uint32_t energyReserved;
    LinkEnergy energy;
};

struct Header {
    std::atomic<std::uint32_t> magic;
    std::atomic<std::uint32_t> version;
    std::uint32_t slotCount;
    std::uint32_t slotStride;
    std::uint8_t session[16];
    std::uint32_t pad[4];
};

struct Segment {
    Header header;
    Slot slots[kLinkMaxInstances];
};

static_assert(std::atomic<std::uint32_t>::is_always_lock_free,
              "the link bus puts atomics in shared memory; a locking atomic would place a "
              "mutex in memory another process cannot use");
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
static_assert(std::is_trivially_copyable_v<Uuid>);
static_assert(std::is_trivially_copyable_v<LinkStatic>);
static_assert(std::is_trivially_copyable_v<LinkDynamic>);
static_assert(std::is_trivially_copyable_v<LinkCommand>);
static_assert(std::is_trivially_copyable_v<LinkControls>);
static_assert(std::is_trivially_copyable_v<LinkPatchEdit>);

//  No padding the compiler chose: every byte of these is a named field, so they copy and compare the same
//  in every build. A failure here means a field was added without accounting for alignment -- and a layout
//  change, which is a kLinkVersion bump.
static_assert(sizeof(LinkNode) == 80);
static_assert(sizeof(LinkCell) == 16);
static_assert(sizeof(LinkTrigger) == 40);
static_assert(sizeof(LinkSourceSlot) == 20);
static_assert(sizeof(LinkRegion) == 4);
/*  24, not 20: the four regions and the quality byte are 20, and genParams is doubles, so the compiler pads to keep
 *  it 8-aligned. Stated as the 24 it costs, since that is the layout this assert exists to pin. */
static_assert(4 * kMaxRegions + 4 <= 24);
static_assert(sizeof(LinkPatch) == 8 + 24 + 8 * kMaxGenParams + 80 * kMaxNodes + 16 * kLinkMaxCells +
                                       40 * kLinkTriggers + 20 * kLinkMaxSources);
static_assert(kNumSources <= kLinkMaxSources);
static_assert(sizeof(LinkControls) == 16 + 4 * kLinkMaxParams + 8 + 2 * kLinkEditNameBytes + sizeof(LinkPatch) - 8);
static_assert(sizeof(LinkPatchEdit) == 24 + 2 * kLinkEditNameBytes + sizeof(LinkPatch));
static_assert(std::tuple_size_v<decltype(PluginState::envTriggers)> == kLinkTriggers);

//  The segment's own layout, not just its payloads. `join()` refuses a slotStride it does not recognise, so a
//  compiler that laid Slot out differently would not corrupt a bus -- it would make the bus silently never join,
//  which is a miserable thing to debug from a user report. Asserted here instead, so a second toolchain that lays
//  it out differently is a compile error. Measured identical on arm64 and x86-64. Slot is the one struct here with
//  padding the compiler chose: four bytes after `editSequence`, since `editLock` is a 64-bit atomic and both
//  targets align it to 8.
static_assert(alignof(Slot) == 8);
static_assert(sizeof(LinkRegionShape) == 8 + 13 * 4 + 4 * kRegionWeights);  // 316, no padding: all 4-byte fields
static_assert(sizeof(LinkEnergy) == 8 + 8 * kLinkEnergyValues);
static_assert(sizeof(Slot) == 87176);  ///< pins the layout; a change here is a kLinkVersion bump
static_assert(sizeof(Header) == 48);
static_assert(sizeof(Segment) == sizeof(Header) + kLinkMaxInstances * sizeof(Slot));

Segment* segmentOf(void* map) { return static_cast<Segment*>(map); }

/*  Seqlock write. Odd means "being written", so a reader that sees an odd count, or a count
 *  that changed across its read, discards what it got and tries again. Only a slot's owner
 *  may call this.
 */
template <typename F>
void seqWrite(std::atomic<std::uint32_t>& seq, F&& write) {
    const std::uint32_t begin = seq.load(std::memory_order_relaxed);
    seq.store(begin + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    write();
    std::atomic_thread_fence(std::memory_order_release);
    seq.store(begin + 2, std::memory_order_relaxed);
}

/*  Seqlock read. Returns false if every attempt collided with a write, in which case what `copy` produced is
 *  unspecified. The retry bound must outlast a write, which takes around a hundred nanoseconds; yielding between
 *  attempts costs nothing here, since readers are UI threads, and widens the retry window to milliseconds. */
template <typename F>
bool seqRead(const std::atomic<std::uint32_t>& seq, F&& copy, std::uint32_t* observed = nullptr) {
    for (int attempt = 0; attempt < 64; ++attempt) {
        if (attempt > 0 && (attempt & 7) == 0) std::this_thread::yield();
        //  Canonical order: acquire on the opening load, the copy, then a fence before re-reading the counter. The
        //  fence must sit after the copy -- it is what stops the second load being hoisted above it.
        const std::uint32_t a = seq.load(std::memory_order_acquire);
        if (a & 1u) continue;
        copy();
        std::atomic_thread_fence(std::memory_order_acquire);
        if (seq.load(std::memory_order_relaxed) == a) {
            if (observed != nullptr) *observed = a;
            return true;
        }
    }
    return false;
}

LinkPoint toPoint(Vec3 v) { return {static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z)}; }

void setText(char* dst, std::size_t size, std::string_view s) {
    std::memset(dst, 0, size);
    std::memcpy(dst, s.data(), std::min(s.size(), size));
}

std::string getText(const char* src, std::size_t size) {
    std::size_t n = 0;
    while (n < size && src[n] != '\0') ++n;
    return std::string(src, n);
}

void putVec(double* d, const Vec3& v) {
    d[0] = v.x;
    d[1] = v.y;
    d[2] = v.z;
}

Vec3 getVec(const double* d) { return {d[0], d[1], d[2]}; }

bool allFinite(const double* d, int n) {
    for (int i = 0; i < n; ++i)
        if (!std::isfinite(d[i])) return false;
    return true;
}

}  // namespace

bool linkHeartbeatAlive(std::uint64_t heartbeat, std::uint64_t now) {
    if (heartbeat == 0) return false;
    if (heartbeat >= now) return true;  // see the note in the header: never subtract here
    return now - heartbeat <= kLinkHeartbeatTimeoutUs;
}

//  One clock for every process on the machine: a clock that differed between them would make the heartbeat
//  reap the living.
std::uint64_t linkNowMicros() { return detail::monotonicMicros(); }

// ------------------------------------------------------------------------------ payloads

std::string LinkStatic::labelString() const {
    std::size_t n = 0;
    while (n < kLinkLabelBytes && label[n] != '\0') ++n;
    return std::string(label, n);
}

void LinkStatic::setLabel(std::string_view s) {
    std::memset(label, 0, sizeof label);
    std::memcpy(label, s.data(), std::min(s.size(), static_cast<std::size_t>(kLinkLabelBytes)));
}

void LinkStatic::setPath(const Trajectory& t) {
    closed = t.closed() ? 1 : 0;
    if (t.empty()) {
        pointCount = 0;
        centre = {0.0f, 0.0f, 1.0f};
        return;
    }
    //  Sampled through eval(), not by striding the lookup table, so this depends only on the
    //  playback contract and not on how the table happens to be laid out.
    const int count = linkPathPointsFor(t.lengthRad());
    const double denom = static_cast<double>(closed ? count : count - 1);
    for (int i = 0; i < count; ++i) path[i] = toPoint(t.eval(static_cast<double>(i) / denom));
    pointCount = static_cast<std::uint32_t>(count);
    centre = toPoint(pathCentre(t.points()));
}

Vec3 LinkStatic::point(int i) const {
    const LinkPoint& p = path[std::clamp(i, 0, kLinkPathPoints - 1)];
    return {p.x, p.y, p.z};
}

Vec3 LinkStatic::centreVec() const { return {centre.x, centre.y, centre.z}; }

void LinkRegionShape::set(const Region& r) {
    kind = static_cast<std::uint8_t>(r.kind);
    sectors = static_cast<std::uint8_t>(std::clamp(r.sectors, 0, 255));
    dots = static_cast<std::uint8_t>(std::clamp(r.dots, 0, 255));
    seed = r.seed;
    yaw = static_cast<float>(r.yaw), pitch = static_cast<float>(r.pitch), roll = static_cast<float>(r.roll);
    softness = static_cast<float>(r.softness), size = static_cast<float>(r.size);
    bandElevation = static_cast<float>(r.bandElevation), thickness = static_cast<float>(r.thickness);
    fill = static_cast<float>(r.fill), dotSize = static_cast<float>(r.dotSize);
    coverage = static_cast<float>(r.coverage), contrast = static_cast<float>(r.contrast);
    detail = static_cast<float>(r.detail), evolve = static_cast<float>(r.evolve);
    for (int i = 0; i < kRegionWeights; ++i) weights[i] = static_cast<float>(r.weights[static_cast<std::size_t>(i)]);
}

Region LinkRegionShape::region() const {
    //  From another process, perhaps another build: every enum clamped, every count kept in range.
    Region r;
    r.kind = static_cast<RegionKind>(std::min<int>(kind, static_cast<int>(RegionKind::Custom)));
    r.sectors = std::max<int>(1, sectors);
    r.dots = dots;
    r.seed = seed;
    r.yaw = yaw, r.pitch = pitch, r.roll = roll, r.softness = softness, r.size = size;
    r.bandElevation = bandElevation, r.thickness = thickness, r.fill = fill, r.dotSize = dotSize;
    r.coverage = coverage, r.contrast = contrast, r.detail = detail, r.evolve = evolve;
    for (int i = 0; i < kRegionWeights; ++i) r.weights[static_cast<std::size_t>(i)] = weights[i];
    return r;
}

PathTransform LinkDynamic::transform() const {
    PathTransform t;
    t.yawRad = yawRad;
    t.pitchRad = pitchRad;
    t.rollRad = rollRad;
    t.extent = extent;
    return t;
}

int linkPathPointsFor(double lengthRad) {
    const double wanted = std::ceil(std::max(lengthRad, 0.0) / kLinkPathStepRad);
    return static_cast<int>(
        std::clamp(wanted, static_cast<double>(kLinkMinPathPoints), static_cast<double>(kLinkPathPoints)));
}

void resolvePath(const LinkStatic& st, const LinkDynamic& dyn, std::vector<Vec3>& out) {
    out.clear();
    const std::uint32_t n = std::min<std::uint32_t>(st.pointCount, kLinkPathPoints);
    const PathTransform t = dyn.transform();
    const Vec3 centre = st.centreVec();
    for (std::uint32_t i = 0; i < n; ++i) out.push_back(applyTransform(st.point(static_cast<int>(i)), centre, t));
}

void LinkPatchEdit::setName(std::string_view s) { setText(name, sizeof name, s); }
void LinkPatchEdit::setKey(std::string_view s) { setText(key, sizeof key, s); }
std::string LinkPatchEdit::nameString() const { return getText(name, sizeof name); }
std::string LinkPatchEdit::keyString() const { return getText(key, sizeof key); }

void LinkControls::setUndoNames(std::string_view undo, std::string_view redo) {
    setText(undoName, sizeof undoName, undo);
    setText(redoName, sizeof redoName, redo);
}
std::string LinkControls::undoNameString() const { return getText(undoName, sizeof undoName); }
std::string LinkControls::redoNameString() const { return getText(redoName, sizeof redoName); }

void packPatch(const PluginState& s, LinkPatch& out) {
    out = LinkPatch{};
    const TrajectoryState& t = s.trajectory;
    out.kind = static_cast<std::uint8_t>(t.kind);
    out.closed = t.closed ? 1 : 0;
    out.generator = static_cast<std::uint8_t>(t.generator);
    for (int i = 0; i < kMaxGenParams; ++i) out.genParams[i] = t.genParams[static_cast<std::size_t>(i)];
    const std::size_t nodes = std::min(t.nodes.size(), static_cast<std::size_t>(kMaxNodes));
    out.nodeCount = static_cast<std::uint8_t>(nodes);
    for (std::size_t i = 0; i < nodes; ++i) {
        putVec(out.nodes[i].p, t.nodes[i].p);
        putVec(out.nodes[i].cin, t.nodes[i].cin);
        putVec(out.nodes[i].cout, t.nodes[i].cout);
        out.nodes[i].smooth = t.nodes[i].smooth ? 1 : 0;
    }

    std::uint32_t cells = 0;
    for (const MatrixCell& c : s.matrix) {
        if (cells == static_cast<std::uint32_t>(kLinkMaxCells)) break;
        if (c.depth == 0.0) continue;
        LinkCell& lc = out.cells[cells++];
        lc.tab = static_cast<std::uint8_t>(c.tab);
        lc.source = static_cast<std::uint8_t>(std::clamp(c.source, 0, 255));
        lc.target = static_cast<std::uint16_t>(c.target);
        lc.depth = c.depth;
    }
    out.cellCount = cells;

    for (int i = 0; i < kLinkTriggers; ++i) {
        const EnvTrigger& e = s.envTriggers[static_cast<std::size_t>(i)];
        LinkTrigger& lt = out.triggers[i];
        lt.input = static_cast<std::uint8_t>(e.input);
        lt.gate = static_cast<std::uint8_t>(e.gate);
        lt.noteLow = static_cast<std::uint8_t>(std::clamp(e.noteLow, 0, 127));
        lt.noteHigh = static_cast<std::uint8_t>(std::clamp(e.noteHigh, 0, 127));
        lt.channel = static_cast<std::uint8_t>(std::clamp(e.channel, 0, 16));
        lt.source = e.source;
        lt.velocity = e.velocity;
        lt.threshold = e.threshold;
        lt.hysteresis = e.hysteresis;
    }
    for (int i = 0; i < kNumSources; ++i) {
        out.sources[i].input = static_cast<std::uint8_t>(s.sources[static_cast<std::size_t>(i)].input);
        out.sources[i].link = s.sources[static_cast<std::size_t>(i)].link;
    }

    //  The effects' half of a patch, carried by every product: one patch type is what lets one bus carry all
    //  three, as one state type does.
    for (int i = 0; i < kMaxRegions; ++i) {
        const RegionShape& r = s.regions[static_cast<std::size_t>(i)].shape;
        out.regions[i].kind = static_cast<std::uint8_t>(r.kind);
        out.regions[i].sectors = static_cast<std::uint8_t>(std::clamp(r.sectors, 0, 255));
        out.regions[i].dots = static_cast<std::uint8_t>(std::clamp(r.dots, 0, 255));
    }
    out.renderQuality = static_cast<std::uint8_t>(s.renderQuality);
}

bool unpackPatch(const ModManifest& m, const LinkPatch& p, PluginState& s) {
    //  Refuse what cannot be repaired, before touching `s`.
    if (p.nodeCount > kMaxNodes || p.cellCount > static_cast<std::uint32_t>(kLinkMaxCells)) return false;
    if (!allFinite(p.genParams, kMaxGenParams)) return false;
    for (int i = 0; i < p.nodeCount; ++i) {
        const LinkNode& n = p.nodes[i];
        if (!allFinite(n.p, 3) || !allFinite(n.cin, 3) || !allFinite(n.cout, 3)) return false;
    }
    for (const LinkTrigger& t : p.triggers)
        if (!std::isfinite(t.velocity) || !std::isfinite(t.threshold) || !std::isfinite(t.hysteresis)) return false;

    TrajectoryState traj;
    traj.kind = p.kind == static_cast<std::uint8_t>(TrajectoryKind::Custom) ? TrajectoryKind::Custom
                                                                            : TrajectoryKind::Parametric;
    traj.closed = p.closed != 0;
    traj.generator = static_cast<GeneratorType>(std::min<int>(p.generator, static_cast<int>(GeneratorType::Spiral)));
    for (int i = 0; i < kMaxGenParams; ++i) traj.genParams[static_cast<std::size_t>(i)] = p.genParams[i];
    traj.nodes.reserve(p.nodeCount);
    for (int i = 0; i < p.nodeCount; ++i) {
        Node n;
        n.p = getVec(p.nodes[i].p);
        n.cin = getVec(p.nodes[i].cin);
        n.cout = getVec(p.nodes[i].cout);
        n.smooth = p.nodes[i].smooth != 0;
        traj.nodes.push_back(n);
    }

    //  setCellDepth checks the target and the column, clamps the depth and collapses duplicates.
    PluginState cells;
    for (std::uint32_t i = 0; i < p.cellCount; ++i) {
        const LinkCell& c = p.cells[i];
        //  Against the receiver's own manifest, never a shared constant: what arrives is an index into the
        //  sender's list, and the two are the same list only while there is one plugin kind.
        if (c.tab > static_cast<std::uint8_t>(MatrixTab::Region) ||
            c.target >= (m.params != nullptr ? m.params->size() : 0) || !std::isfinite(c.depth))
            continue;
        setCellDepth(m, cells, static_cast<MatrixTab>(c.tab), c.source, static_cast<ParamId>(c.target), c.depth);
    }

    std::array<EnvTrigger, kLinkTriggers> triggers{};
    for (int i = 0; i < kLinkTriggers; ++i) {
        const LinkTrigger& lt = p.triggers[i];
        EnvTrigger e;
        e.input = lt.input == static_cast<std::uint8_t>(TriggerInput::Audio) ? TriggerInput::Audio : TriggerInput::Midi;
        e.gate = lt.gate == static_cast<std::uint8_t>(TriggerGate::OneShot) ? TriggerGate::OneShot : TriggerGate::Held;
        setTriggerHigh(e, 127);  // so the low end lands first, and a reversed pair closes up
        setTriggerLow(e, lt.noteLow);
        setTriggerHigh(e, lt.noteHigh);
        setTriggerChannel(e, lt.channel);
        setTriggerVelocity(e, lt.velocity);
        setTriggerThreshold(e, lt.threshold);
        e.hysteresis = 0.0;
        setTriggerHysteresis(e, lt.hysteresis);
        setTriggerSource(e, lt.source);
        triggers[static_cast<std::size_t>(i)] = e;
    }

    std::array<SourceSlot, kNumSources> sources{};
    for (int i = 0; i < kNumSources; ++i) {
        sources[static_cast<std::size_t>(i)].input =
            static_cast<SourceInput>(std::min<int>(p.sources[i].input, static_cast<int>(SourceInput::Link)));
        sources[static_cast<std::size_t>(i)].link = p.sources[i].link;
    }

    s.trajectory = std::move(traj);
    s.matrix = std::move(cells.matrix);
    s.envTriggers = triggers;
    s.sources = sources;

    /*  The regions and the render quality. Repaired rather than refused, as everything else here
        is, and repaired by `sanitised()` -- the one place that decides what an evaluable shape is,
        so a region that crossed the bus cannot differ from one that came out of a state file. */
    for (int i = 0; i < kMaxRegions; ++i) {
        const LinkRegion& lr = p.regions[i];
        RegionShape shape;
        shape.kind = static_cast<RegionKind>(std::clamp<int>(lr.kind, 0, static_cast<int>(RegionKind::Custom)));
        shape.sectors = lr.sectors;
        shape.dots = lr.dots;
        s.regions[static_cast<std::size_t>(i)].shape = sanitised(shape);
    }
    s.renderQuality = p.renderQuality == static_cast<std::uint8_t>(RenderQuality::Same) ? RenderQuality::Same
                                                                                        : RenderQuality::Realistic;

    return true;
}

ParamId LinkCommand::paramId() const {
    /*  The raw index, as a ParamId, not validated here: a command is a POD in shared memory with no manifest to
     *  check against, and the only manifest that means anything is the receiver's -- whoever applies it checks
     *  against its own list (`ParamManifest::has`). */
    return param > 0xFFFFu ? kNoParamId : static_cast<ParamId>(param);
}

// -------------------------------------------------------------------------------- LinkBus

std::string LinkBus::segmentNameFor(const Uuid& session) {
    //  macOS caps shared-memory names at 31 characters including the leading slash, which is shorter than a
    //  full uuid renders. The first 12 hex digits are 48 bits of a random uuid: collision across two open
    //  projects is not a practical concern. The layout version is in the name for the reason given in the
    //  header -- a segment outlives its process, and is sizeable only once.
    const std::string hex = session.toString();
    std::string compact;
    for (char c : hex) {
        if (c == '-') continue;
        compact.push_back(c);
        if (compact.size() == 12) break;
    }
    return "/bambi." + std::to_string(kLinkVersion) + "." + compact;
}

LinkBus::~LinkBus() { close(); }

bool LinkBus::open(const Uuid& session, const Uuid& instance, Product product) {
    hasStatic_ = false;
    hasControls_ = false;
    rejoins_ = 0;
    return join(session, instance, product);
}

bool LinkBus::join(const Uuid& session, const Uuid& instance, Product product) {
    close();
    error_.clear();
    instanceConflict_ = false;
    session_ = session;
    instance_ = instance;
    product_ = product;
    name_ = segmentNameFor(session);
    mapBytes_ = sizeof(Segment);

    //  Opening, sizing and mapping tolerate two instances creating the segment at once, and
    //  never map more than the object holds. Shared with the session directory.
    if (!detail::mapShared(name_, mapBytes_, handle_, map_, error_)) return false;
    Segment* seg = segmentOf(map_);

    /*  Initialise exactly once, whoever gets there first. A fresh POSIX segment is
     *  zero-filled, so "magic is 0" identifies it; the CAS makes the winner unambiguous when
     *  several instances open simultaneously. */
    std::uint32_t expected = 0;
    if (seg->header.magic.compare_exchange_strong(expected, kMagic, std::memory_order_acq_rel)) {
        seg->header.slotCount = kLinkMaxInstances;
        seg->header.slotStride = sizeof(Slot);
        std::memcpy(seg->header.session, session.bytes.data(), 16);
        seg->header.version.store(kLinkVersion, std::memory_order_release);
    } else if (expected != kMagic) {
        error_ = "shared segment is not an bambi bus";
        close();
        return false;
    } else {
        /*  Someone else created it. Check that they laid it out the way this build expects
         *  before trusting a single field.
         *
         *  This is a plugin: two bambi versions can be loaded into one project, from two
         *  different installs, and the older one will happily map a segment the newer one
         *  made. Reading a struct that has moved underneath you is not an error that
         *  reports itself -- it is garbage positions, or a crash inside the host. Refusing
         *  to join is the correct failure, because every caller already has to work with no
         *  bus at all.
         *
         *  The creator sets the layout fields before releasing `version`, so waiting on
         *  version is what makes reading the rest safe. */
        //  Short, for the reason mapShared gives: waiting here freezes the host's message thread.
        for (int attempt = 0; attempt < 40; ++attempt) {
            if (seg->header.version.load(std::memory_order_acquire) != 0) break;
            std::this_thread::sleep_for(std::chrono::microseconds(500));
        }
        const std::uint32_t ver = seg->header.version.load(std::memory_order_acquire);
        if (ver != kLinkVersion || seg->header.slotStride != sizeof(Slot) ||
            seg->header.slotCount != kLinkMaxInstances) {
            error_ = "session bus was created by an incompatible build (version " + std::to_string(ver) + ", stride " +
                     std::to_string(seg->header.slotStride) + "); this build expects version " +
                     std::to_string(kLinkVersion) + ", stride " + std::to_string(sizeof(Slot));
            close();
            return false;
        }
    }

    //  Reclaim anything abandoned before looking for a free slot, so a host that crashed and
    //  restarted does not leak its way to a full bus.
    reapDead();

    for (int i = 0; i < kLinkMaxInstances; ++i) {
        std::uint32_t free = 0;
        if (seg->slots[i].occupied.compare_exchange_strong(free, 1, std::memory_order_acq_rel)) {
            Slot& s = seg->slots[i];
            //  Zero first: this slot is now being claimed, and a reaper must leave it alone (reapDead).
            s.heartbeat.store(0, std::memory_order_release);
            //  Everything is reset before the heartbeat is stamped. Readers only treat a slot
            //  as a live peer once its heartbeat is fresh, so nobody can see this instance
            //  wearing a previous occupant's identity, path or position.
            s.dynSeq.store(0, std::memory_order_relaxed);
            s.staticSeq.store(0, std::memory_order_relaxed);
            s.staticGeneration.store(0, std::memory_order_relaxed);
            s.staticGenerationPayload = 0;
            s.head.store(0, std::memory_order_relaxed);
            s.tail.store(0, std::memory_order_relaxed);
            s.dropped.store(0, std::memory_order_relaxed);
            s.controlsSeq.store(0, std::memory_order_relaxed);
            s.controlsGeneration.store(0, std::memory_order_relaxed);
            s.controlsGenerationPayload = 0;
            s.editSeq.store(0, std::memory_order_relaxed);
            s.editSequence.store(0, std::memory_order_relaxed);
            s.editLock.store(0, std::memory_order_relaxed);
            for (auto& cell : s.inbox) cell.ready.store(0, std::memory_order_relaxed);
            s.energyAskedUs.store(0, std::memory_order_relaxed);
            s.energySeq.store(0, std::memory_order_relaxed);
            s.energyGeneration.store(0, std::memory_order_relaxed);
            s.energyGenerationPayload = 0;
            s.dyn = LinkDynamic{};
            s.st = LinkStatic{};
            s.instance = instance;
            s.product = static_cast<std::uint32_t>(product);
            s.heartbeat.store(linkNowMicros(), std::memory_order_release);
            slot_ = i;

            /*  Is this id already live in another slot? Checked after our own heartbeat is
             *  stamped, which is what makes the race safe: of two duplicates joining at once,
             *  whichever looks second sees the other. Both may see each other, and then both
             *  step back and take fresh ids -- harmless. Neither can miss the other, so two
             *  slots never keep one id. The instance already live keeps it, so references to
             *  it elsewhere stay valid. */
            const std::uint64_t now = linkNowMicros();
            for (int j = 0; j < kLinkMaxInstances; ++j) {
                if (j == i) continue;
                const Slot& other = seg->slots[j];
                if (other.occupied.load(std::memory_order_acquire) == 0) continue;
                if (!linkHeartbeatAlive(other.heartbeat.load(std::memory_order_acquire), now)) continue;
                if (other.instance == instance) {
                    instanceConflict_ = true;
                    error_ = "another live instance in this session already uses this instance id";
                    close();  // releases the slot just claimed
                    return false;
                }
            }
            return true;
        }
    }

    error_ = "no free slot: the session already has " + std::to_string(kLinkMaxInstances) + " instances";
    close();
    return false;
}

void LinkBus::close() {
    if (map_ != nullptr) {
        //  Release the slot rather than letting the heartbeat lapse, so a cleanly closed
        //  plugin disappears from other windows immediately instead of in two seconds.
        //  But only a slot that is still ours: an instance that was reaped while suspended
        //  and whose slot has since been claimed must not evict the newcomer on its way out.
        if (slot_ >= 0 && ownsSlot()) {
            Slot& s = segmentOf(map_)->slots[slot_];
            s.heartbeat.store(0, std::memory_order_relaxed);
            s.occupied.store(0, std::memory_order_release);
        }
    }
    detail::unmapShared(map_, mapBytes_, handle_);
    slot_ = -1;
    mapBytes_ = 0;
}

bool LinkBus::ownsSlot() const {
    const Slot& s = segmentOf(map_)->slots[slot_];
    return s.occupied.load(std::memory_order_acquire) != 0 && s.instance == instance_;
}

/*  Our slot was reclaimed while we were still running, and we have noticed.
 *
 *  This happens to an ordinary process: a laptop lid closed, a debugger breakpoint, a host
 *  that stalls for a few seconds. Past the heartbeat timeout any other instance that opens
 *  the bus reaps the slot, and a newcomer may claim it. Carrying on would be wrong both ways
 *  -- writing into a slot marked free makes us invisible forever, and writing into one that
 *  someone else now owns breaks the single-writer rule every seqlock here depends on.
 *
 *  So join again, as the same instance, into whatever slot is free, and republish the last
 *  static section so the path is not lost with the old slot. The one case this cannot guard
 *  is being suspended inside a write; that window is nanoseconds wide.
 */
bool LinkBus::rejoin() {
    const Uuid session = session_, instance = instance_;
    const Product product = product_;
    if (!join(session, instance, product)) return false;
    ++rejoins_;
    if (hasStatic_) writeStatic(lastStatic_);
    if (hasControls_) writeControls(lastControls_);
    return true;
}

void LinkBus::publishStatic(const LinkStatic& st) {
    if (map_ == nullptr || slot_ < 0) return;
    lastStatic_ = st;
    hasStatic_ = true;
    if (!ownsSlot()) {
        rejoin();  // republishes lastStatic_, which is now `st`
        return;
    }
    writeStatic(st);
}

void LinkBus::writeStatic(const LinkStatic& st) {
    Slot& s = segmentOf(map_)->slots[slot_];
    std::uint32_t gen = s.staticGenerationPayload + 1;
    if (gen == 0) gen = 1;  // 0 means "never published"; a wrapped counter must not claim it
    //  The generation is written inside the lock, with the path. Outside it -- even one line
    //  away -- a reader can pair a path with the wrong number and the scene caches that pair
    //  indefinitely. The window is one instruction wide, so no test can see it (mutation-
    //  checked); this comment is the guard.
    seqWrite(s.staticSeq, [&] {
        s.st = st;
        s.staticGenerationPayload = gen;
    });
    s.staticGeneration.store(gen, std::memory_order_release);
}

void LinkBus::publishControls(const LinkControls& c) {
    if (map_ == nullptr || slot_ < 0) return;
    lastControls_ = c;
    hasControls_ = true;
    if (!ownsSlot()) {
        rejoin();  // republishes lastControls_, which is now `c`
        return;
    }
    writeControls(c);
}

void LinkBus::writeControls(const LinkControls& c) {
    Slot& s = segmentOf(map_)->slots[slot_];
    std::uint32_t gen = s.controlsGenerationPayload + 1;
    if (gen == 0) gen = 1;
    //  The generation goes in with the content, for the reason writeStatic gives.
    seqWrite(s.controlsSeq, [&] {
        s.controls = c;
        s.controlsGenerationPayload = gen;
    });
    s.controlsGeneration.store(gen, std::memory_order_release);
}

void LinkBus::publishDynamic(const LinkDynamic& d) {
    if (map_ == nullptr || slot_ < 0) return;
    if (!ownsSlot() && !rejoin()) return;
    Slot& s = segmentOf(map_)->slots[slot_];
    seqWrite(s.dynSeq, [&] { s.dyn = d; });
    s.heartbeat.store(linkNowMicros(), std::memory_order_release);
}

void LinkBus::poll(std::vector<LinkPeer>& out) const {
    out.clear();
    if (map_ == nullptr) return;
    Segment* seg = segmentOf(map_);
    const std::uint64_t now = linkNowMicros();

    for (int i = 0; i < kLinkMaxInstances; ++i) {
        Slot& s = seg->slots[i];
        if (s.occupied.load(std::memory_order_acquire) == 0) continue;
        if (!linkHeartbeatAlive(s.heartbeat.load(std::memory_order_relaxed), now)) continue;

        LinkPeer p;
        p.slot = i;
        p.instance = s.instance;
        p.product = static_cast<Product>(s.product);
        p.staticGeneration = s.staticGeneration.load(std::memory_order_acquire);
        p.controlsGeneration = s.controlsGeneration.load(std::memory_order_acquire);
        std::uint32_t observed = 0;
        if (seqRead(s.dynSeq, [&] { p.dyn = s.dyn; }, &observed)) {
            //  A sequence of 0 means no publish has ever completed, so what was copied is the
            //  default join() wrote -- not a position. Exact, and free: the counter is
            //  already in hand.
            //  And only an encoder has a source: an effect publishes its sources and regions here too,
            //  and has no position to draw.
            p.hasPosition = observed != 0 && p.product == Product::Encoder;
            out.push_back(p);
        }
    }
}

bool LinkBus::readStatic(const LinkPeer& peer, LinkStatic& out, std::uint32_t& generation) const {
    if (map_ == nullptr || peer.slot < 0 || peer.slot >= kLinkMaxInstances) return false;
    Slot& s = segmentOf(map_)->slots[peer.slot];

    //  Checked on both sides of the copy. A peer that closed, and a newcomer that claimed the
    //  slot within those few microseconds, would otherwise hand back one instance's path under
    //  another's name.
    const auto stillThem = [&] {
        return s.occupied.load(std::memory_order_acquire) != 0 &&
               linkHeartbeatAlive(s.heartbeat.load(std::memory_order_relaxed), linkNowMicros()) &&
               s.instance == peer.instance;
    };
    if (!stillThem()) return false;

    std::uint32_t gen = 0;
    if (!seqRead(s.staticSeq, [&] {
            out = s.st;
            gen = s.staticGenerationPayload;
        }))
        return false;
    if (gen == 0 || !stillThem()) return false;
    generation = gen;
    return true;
}

bool LinkBus::readControls(const LinkPeer& peer, LinkControls& out, std::uint32_t& generation) const {
    if (map_ == nullptr || peer.slot < 0 || peer.slot >= kLinkMaxInstances) return false;
    Slot& s = segmentOf(map_)->slots[peer.slot];
    const auto stillThem = [&] {
        return s.occupied.load(std::memory_order_acquire) != 0 &&
               linkHeartbeatAlive(s.heartbeat.load(std::memory_order_relaxed), linkNowMicros()) &&
               s.instance == peer.instance;
    };
    if (!stillThem()) return false;

    std::uint32_t gen = 0;
    if (!seqRead(s.controlsSeq, [&] {
            out = s.controls;
            gen = s.controlsGenerationPayload;
        }))
        return false;
    if (gen == 0 || !stillThem()) return false;
    generation = gen;
    return true;
}

void LinkBus::publishEnergy(const LinkEnergy& e) {
    if (map_ == nullptr || slot_ < 0 || !ownsSlot()) return;  // not kept for a rejoin: the next one comes soon
    Slot& s = segmentOf(map_)->slots[slot_];
    std::uint32_t gen = s.energyGenerationPayload + 1;
    if (gen == 0) gen = 1;
    seqWrite(s.energySeq, [&] {
        s.energy = e;
        s.energyGenerationPayload = gen;
    });
    s.energyGeneration.store(gen, std::memory_order_release);
}

bool LinkBus::readEnergy(const LinkPeer& peer, LinkEnergy& out, std::uint32_t& generation) const {
    if (map_ == nullptr || peer.slot < 0 || peer.slot >= kLinkMaxInstances) return false;
    Slot& s = segmentOf(map_)->slots[peer.slot];
    const auto stillThem = [&] {
        return s.occupied.load(std::memory_order_acquire) != 0 &&
               linkHeartbeatAlive(s.heartbeat.load(std::memory_order_relaxed), linkNowMicros()) &&
               s.instance == peer.instance;
    };
    if (!stillThem()) return false;
    std::uint32_t gen = 0;
    if (!seqRead(s.energySeq, [&] {
            out = s.energy;
            gen = s.energyGenerationPayload;
        }))
        return false;
    if (gen == 0 || !stillThem()) return false;
    //  from another process: the order and the count are clamped to what the arrays hold
    out.order = std::min<std::uint8_t>(out.order, kLinkEnergyOrder);
    out.count = std::min<std::uint32_t>(out.count, kLinkEnergyValues);
    generation = gen;
    return true;
}

bool LinkBus::askForEnergy(const LinkPeer& peer, std::uint64_t untilUs) {
    if (map_ == nullptr || peer.slot < 0 || peer.slot >= kLinkMaxInstances) return false;
    Slot& s = segmentOf(map_)->slots[peer.slot];
    if (s.occupied.load(std::memory_order_acquire) == 0 || !(s.instance == peer.instance)) return false;
    //  the latest request wins; several viewers each keep it fresh
    std::uint64_t now = s.energyAskedUs.load(std::memory_order_relaxed);
    while (now < untilUs && !s.energyAskedUs.compare_exchange_weak(now, untilUs, std::memory_order_relaxed)) {}
    return true;
}

std::uint64_t LinkBus::energyAskedUntil() const {
    if (map_ == nullptr || slot_ < 0) return 0;
    return segmentOf(map_)->slots[slot_].energyAskedUs.load(std::memory_order_relaxed);
}

int LinkBus::reapDead() {
    if (map_ == nullptr) return 0;
    Segment* seg = segmentOf(map_);
    const std::uint64_t now = linkNowMicros();
    int reaped = 0;
    for (int i = 0; i < kLinkMaxInstances; ++i) {
        Slot& s = seg->slots[i];
        if (s.occupied.load(std::memory_order_acquire) == 0) continue;
        if (i == slot_) continue;
        std::uint64_t hb = s.heartbeat.load(std::memory_order_acquire);
        //  A zero heartbeat on an occupied slot is an instance in the middle of join(): claimed, not
        //  yet stamped. Reaping it handed the slot to a second joiner while the first was still
        //  writing it -- two instances in one slot, found by a stress test of simultaneous joins.
        //  (A joiner that dies inside those few instructions leaks the slot; that is the cheaper
        //  failure.)
        if (hb == 0 || linkHeartbeatAlive(hb, now)) continue;
        //  Clear the heartbeat by CAS before releasing: a slot refreshed since the load is left
        //  alone, and a released slot always carries a zero heartbeat for its next claimer.
        if (!s.heartbeat.compare_exchange_strong(hb, 0, std::memory_order_acq_rel)) continue;
        std::uint32_t taken = 1;
        if (s.occupied.compare_exchange_strong(taken, 0, std::memory_order_acq_rel)) ++reaped;
    }
    return reaped;
}

bool LinkBus::sendCommand(const Uuid& target, const LinkCommand& c) {
    if (map_ == nullptr) return false;
    Segment* seg = segmentOf(map_);
    const std::uint64_t now = linkNowMicros();

    for (int i = 0; i < kLinkMaxInstances; ++i) {
        Slot& s = seg->slots[i];
        if (s.occupied.load(std::memory_order_acquire) == 0) continue;
        if (!linkHeartbeatAlive(s.heartbeat.load(std::memory_order_relaxed), now)) continue;
        //  Read without a seqlock on purpose: identity is written once in join(), before the
        //  heartbeat that makes the slot visible, and never changes after. The product is written
        //  with it and is read the same way.
        if (!(s.instance == target)) continue;
        //  `param` is an index into a manifest, and a manifest is a plugin's own: the same index means a different
        //  parameter in another kind, so this is refused rather than applied in silence. The scene still sees
        //  every kind -- nothing drawn is addressed by position.
        if (s.product != static_cast<std::uint32_t>(product_)) {
            s.dropped.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        /*  Many senders, one receiver. Producers claim an index by CAS on head; the owner moves tail. A full inbox
         *  refuses rather than blocks -- a UI thread must never wait on another process's scheduling -- and tells
         *  the sender. A refusal is harmless for a stream of values, since each supersedes the last, but not for a
         *  gesture's begin or end, so the last kLinkInboxReserved slots are theirs alone: values stop fitting
         *  first, while the endpoints carrying the press and release values still do. */
        const std::uint32_t capacity = static_cast<std::uint32_t>(
            c.gesture == kLinkGestureNone ? kLinkInboxSlots - kLinkInboxReserved : kLinkInboxSlots);
        for (int attempt = 0; attempt < 64; ++attempt) {
            std::uint32_t head = s.head.load(std::memory_order_relaxed);
            const std::uint32_t tail = s.tail.load(std::memory_order_acquire);
            if (head - tail >= capacity) {
                s.dropped.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            //  compare_exchange writes the observed value back into `head` on failure, so
            //  the retry re-reads tail as well rather than looping on a stale fullness test.
            const std::uint32_t claimed = head;
            if (s.head.compare_exchange_weak(head, claimed + 1, std::memory_order_acq_rel)) {
                LinkCommand copy = c;
                copy.from = instance_;
                InboxCell& cell = s.inbox[claimed % kLinkInboxSlots];
                cell.cmd = copy;
                //  Publish last. Everything above must be visible to the consumer before
                //  the cell is announced as holding index `claimed`.
                cell.ready.store(claimed + 1, std::memory_order_release);
                return true;
            }
        }
        return false;
    }
    return false;
}

std::uint32_t LinkBus::sendPatchEdit(const Uuid& target, const LinkPatchEdit& edit) {
    if (map_ == nullptr) return 0;
    Segment* seg = segmentOf(map_);
    const std::uint64_t now = linkNowMicros();

    for (int i = 0; i < kLinkMaxInstances; ++i) {
        Slot& s = seg->slots[i];
        if (s.occupied.load(std::memory_order_acquire) == 0) continue;
        if (!linkHeartbeatAlive(s.heartbeat.load(std::memory_order_relaxed), now)) continue;
        if (!(s.instance == target)) continue;
        //  A patch is shaped like its plugin's own state entirely, so it means nothing in another kind.
        if (s.product != static_cast<std::uint32_t>(product_)) return 0;

        for (int attempt = 0; attempt < 64; ++attempt) {
            if (attempt > 0 && (attempt & 7) == 0) std::this_thread::yield();
            std::uint64_t held = s.editLock.load(std::memory_order_acquire);
            const std::uint64_t at = linkNowMicros();
            //  Free, or left by a sender that died holding it. A lock stamped after `at` is fresh --
            //  never subtract into the future (linkHeartbeatAlive has the same guard).
            const bool stale = held != 0 && held < at && at - held > kLinkEditLockStaleUs;
            if (held != 0 && !stale) continue;
            if (!s.editLock.compare_exchange_strong(held, at, std::memory_order_acq_rel)) continue;

            //  Holding the lock, this is the cell's one writer, which the seqlock needs.
            std::uint32_t sequence = s.editSequence.load(std::memory_order_relaxed) + 1;
            if (sequence == 0) sequence = 1;
            seqWrite(s.editSeq, [&] {
                s.edit = edit;
                s.edit.from = instance_;  // stamped here, never taken from the sender's copy
                s.edit.sequence = sequence;
            });
            s.editSequence.store(sequence, std::memory_order_release);
            std::uint64_t mine = at;
            s.editLock.compare_exchange_strong(mine, 0,
                                               std::memory_order_acq_rel);  // a lock taken from us is not ours to free
            return sequence;
        }
        return 0;
    }
    return 0;
}

bool LinkBus::takePatchEdit(LinkPatchEdit& out, std::uint32_t& lastSequence) {
    if (map_ == nullptr || slot_ < 0 || !ownsSlot()) return false;
    Slot& s = segmentOf(map_)->slots[slot_];
    if (s.editSequence.load(std::memory_order_acquire) == lastSequence) return false;
    if (!seqRead(s.editSeq, [&] { out = s.edit; })) return false;
    if (out.sequence == 0 || out.sequence == lastSequence) return false;
    lastSequence = out.sequence;
    return true;
}

void LinkBus::drainCommands(std::vector<LinkCommand>& out) {
    out.clear();
    if (map_ == nullptr || slot_ < 0) return;
    //  A slot taken from us holds someone else's inbox. The next publish rejoins.
    if (!ownsSlot()) return;
    Slot& s = segmentOf(map_)->slots[slot_];

    std::uint32_t tail = s.tail.load(std::memory_order_relaxed);
    const std::uint32_t head = s.head.load(std::memory_order_acquire);

    /*  Stop at the first entry that is claimed but not yet published, rather than skipping
     *  it. Order matters for a gesture (begin, value, value, end), so a hole cannot be
     *  stepped over -- the next drain will pick it up microseconds later. */
    while (tail != head) {
        InboxCell& cell = s.inbox[tail % kLinkInboxSlots];
        if (cell.ready.load(std::memory_order_acquire) != tail + 1) break;
        out.push_back(cell.cmd);
        ++tail;
    }
    s.tail.store(tail, std::memory_order_release);
}

int LinkBus::slotCount() const {
    if (map_ == nullptr) return 0;
    Segment* seg = segmentOf(map_);
    const std::uint64_t now = linkNowMicros();
    int n = 0;
    for (int i = 0; i < kLinkMaxInstances; ++i) {
        const Slot& s = seg->slots[i];
        if (s.occupied.load(std::memory_order_acquire) == 0) continue;
        if (linkHeartbeatAlive(s.heartbeat.load(std::memory_order_relaxed), now)) ++n;
    }
    return n;
}

void LinkBus::unlinkSession(const Uuid& session) { detail::unlinkShared(segmentNameFor(session)); }

// -------------------------------------------------------------------------- LinkPublisher

void LinkPublisher::tick() {
    latest_.read(last_);  // the newest submit, if one arrived; otherwise last_ is kept
    last_.publishedUs = linkNowMicros();
    bus_.publishDynamic(last_);
}

// ------------------------------------------------------------------------------ LinkScene

void LinkScene::update(const LinkBus& bus) {
    bus.poll(peers_);

    //  Entries are updated in place, not rebuilt: each carries a path of up to 49 KB, and rebuilding
    //  the vector every frame would copy every one of them to learn nothing.
    seen_.assign(entries_.size(), 0);
    for (const LinkPeer& p : peers_) {
        std::size_t k = 0;
        while (k < entries_.size() && !(entries_[k].instance == p.instance)) ++k;
        if (k == entries_.size()) {
            entries_.emplace_back();
            entries_.back().instance = p.instance;
            seen_.push_back(0);
        }
        Entry& e = entries_[k];
        seen_[k] = 1;
        e.dyn = p.dyn;
        e.product = p.product;
        e.hasPosition = p.hasPosition;
        e.slot = p.slot;
        e.controlsGeneration = p.controlsGeneration;

        if (p.staticGeneration != 0 && p.staticGeneration != e.generation) {
            //  Into scratch, never straight into the entry: a failed read leaves its copy
            //  unspecified, and a stale-but-whole path is better than a torn one.
            std::uint32_t gen = 0;
            if (bus.readStatic(p, scratch_, gen)) {
                e.st = scratch_;
                e.generation = gen;
                ++staticReads_;
            }
        }
    }

    std::size_t w = 0;
    for (std::size_t r = 0; r < entries_.size(); ++r) {
        if (seen_[r] == 0) continue;
        if (w != r) entries_[w] = entries_[r];
        ++w;
    }
    entries_.resize(w);
}

std::string LinkScene::Entry::label() const {
    const std::string published = generation > 0 ? st.labelString() : std::string();
    return published.empty() ? "instance " + instance.toString().substr(0, 4) : published;
}

LinkPeer LinkScene::Entry::peer() const {
    LinkPeer p;
    p.instance = instance;
    p.product = product;
    p.slot = slot;
    p.staticGeneration = generation;
    p.controlsGeneration = controlsGeneration;
    p.dyn = dyn;
    p.hasPosition = hasPosition;
    return p;
}

const LinkScene::Entry* LinkScene::find(const Uuid& instance) const {
    for (const auto& e : entries_)
        if (e.instance == instance) return &e;
    return nullptr;
}

// ----------------------------------------------------------------------- LinkEditReceiver

void LinkEditReceiver::apply(std::span<const LinkCommand> commands, std::vector<LinkEditAction>& out) {
    using K = LinkEditAction::Kind;
    for (const LinkCommand& c : commands) {
        const ParamId id = c.paramId();
        if (id == kNoParamId) continue;  // a parameter this build does not have
        //  A command is a POD in memory another process writes: its index is held to what is kept
        //  here before it is used as one. `paramId` lets anything up to 0xFFFF through.
        if (static_cast<std::size_t>(id) >= gestures_.size()) continue;
        Gesture& g = gestures_[static_cast<std::size_t>(id)];
        const bool ours = g.open && g.owner == c.from;

        switch (c.gesture) {
            case kLinkGestureBegin:
                //  Never nest. Whoever held it -- this sender beginning again, or another
                //  window -- is ended first. Last begin wins.
                if (g.open) out.push_back({K::End, id});
                out.push_back({K::Begin, id});
                out.push_back({K::Value, id, c.value});
                g.owner = c.from;
                g.open = true;
                break;

            case kLinkGestureEnd:
                if (ours) {
                    out.push_back({K::Value, id, c.value});  // where the user let go
                    out.push_back({K::End, id});
                    g.open = false;
                } else if (!g.open) {
                    //  Its begin never arrived; the release value still has to land.
                    out.push_back({K::Begin, id});
                    out.push_back({K::Value, id, c.value});
                    out.push_back({K::End, id});
                }
                //  Otherwise another window took over mid-gesture, and this release is stale.
                break;

            default:
                if (ours) {
                    out.push_back({K::Value, id, c.value});
                } else if (!g.open) {
                    out.push_back({K::Begin, id});
                    out.push_back({K::Value, id, c.value});
                    out.push_back({K::End, id});
                }
                //  Otherwise a superseded sender still dragging: ignored.
                break;
        }
    }
}

void LinkEditReceiver::closeAbandoned(std::span<const LinkPeer> live, std::vector<LinkEditAction>& out) {
    for (std::size_t i = 0; i < gestures_.size(); ++i) {
        Gesture& g = gestures_[i];
        if (!g.open) continue;
        bool alive = false;
        for (const LinkPeer& p : live)
            if (p.instance == g.owner) alive = true;
        if (alive) continue;
        LinkEditAction a;
        a.kind = LinkEditAction::Kind::End;
        a.param = static_cast<ParamId>(i);
        a.abandoned = true;
        out.push_back(a);
        g.open = false;
    }
}

bool LinkEditReceiver::isOpen(ParamId id) const {
    return id != kNoParamId && gestures_[static_cast<std::size_t>(id)].open;
}

std::size_t LinkEditReceiver::openGestures() const {
    std::size_t n = 0;
    for (const auto& g : gestures_) n += g.open ? 1 : 0;
    return n;
}

}  // namespace bambi
