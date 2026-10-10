// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "bambi/patch/identity.hpp"
#include "bambi/patch/json.hpp"
#include "bambi/patch/layout.hpp"
#include "bambi/patch/parameters.hpp"
#include "bambi/path/shape.hpp"  // a patch contains a trajectory; path/ never includes patch/
#include "bambi/region/shape.hpp"

namespace bambi {

/// Plugin state -- everything that is not a host parameter. Bump the version only for a schema
/// change; adding or removing a parameter is not one, because everything serialises by name
/// (parameter key, enum string, generator parameter name, never an ordinal or array position),
/// so an older document loads with new fields at their defaults and a newer one ignores fields
/// it doesn't recognise. Per product: a document says which plugin wrote it, since "version 2"
/// means a different change in each.
int stateVersionFor(Product p);

inline constexpr int kEncoderStateVersion = 1;
inline constexpr int kEffectStateVersion = 1;
inline constexpr int kNumSources = 19;  ///< one per mod.amount.* parameter
/// What an offline render does when a plugin offers a quality switch: follow the live setting,
/// or always use the better one. Not a host parameter, since nobody rides it in a mix.
enum class RenderQuality { Same, Realistic };

enum class SourceInput { Self, Sidechain, Link };
enum class MatrixTab { Features, Sidechain, Generators, Region };
enum class TriggerInput { Midi, Audio };
enum class TriggerGate { Held, OneShot };

struct MatrixCell {
    MatrixTab tab{MatrixTab::Features};
    int source{0};  ///< column index within the tab
    ParamId target{kNoParamId};
    double depth{0.0};

    friend bool operator==(const MatrixCell&, const MatrixCell&) = default;
};

/// What fires a generator envelope: a patch decision, not something ridden in a mix, so it's
/// state rather than a host parameter. MIDI by default, each envelope listening to its own note
/// or range on one channel or any; defaults follow the General MIDI drum map (C1 kick, D1 snare,
/// F#1 closed hi-hat) so a pad works unmodified. `held` keeps the gate open while a matching note
/// is down; `one-shot` runs attack/decay/release once and ignores note-off, for pads that send
/// very short notes. Audio fires from a source crossing a threshold, with hysteresis; both halves
/// persist regardless of the active input, so switching back and forth loses nothing.
struct EnvTrigger {
    TriggerInput input{TriggerInput::Midi};

    // midi
    int noteLow{36};   ///< inclusive, 0..127
    int noteHigh{36};  ///< inclusive, 0..127
    int channel{0};    ///< 0 = any, else 1..16
    TriggerGate gate{TriggerGate::OneShot};
    double velocity{0.0};  ///< 0 = every hit alike; 1 = the envelope scales fully with velocity

    // audio
    int source{1};  ///< source slot; 1 = self Attack
    double threshold{0.35};
    double hysteresis{0.10};  ///< releases below (threshold - hysteresis); stops chatter

    /// The default for envelope `index`: its General MIDI drum-map note.
    static EnvTrigger defaults(int index);

    friend bool operator==(const EnvTrigger&, const EnvTrigger&) = default;
};

struct SourceSlot {
    SourceInput input{SourceInput::Self};
    Uuid link{};  ///< instance uuid when input == Link

    friend bool operator==(const SourceSlot&, const SourceSlot&) = default;
};

/// A region's kind and counts -- what it is, not its settings (host parameters in region1.*).
/// Not automatable: changing kind or count changes what those settings mean.
struct RegionEntry {
    RegionShape shape;

    friend bool operator==(const RegionEntry&, const RegionEntry&) = default;
};

/// Which preset a patch came from: the header name, nothing the engine reads. Saved with the
/// session so an undo of a load restores the previous name. A user preset sits in one folder deep.
/// Where a starting point's values came from (Reverb's room, Echo's pattern), as an index into
/// the name tables below; `shape` also carries the room's shape. Written only when not default.
inline constexpr std::array<std::string_view, 6> kRoomNames{"ambience", "room",       "chamber",
                                                            "hall",     "large hall", "cathedral"};
inline constexpr std::array<std::string_view, 3> kRoomShapeNames{"room", "hall", "tall"};
inline constexpr std::array<std::string_view, 5> kEchoPatternNames{"even", "ping-pong", "spiral", "cascade", "single"};

/// A large text held by value, copied by reference: equal when the texts are, however they are held.
struct SharedText {
    std::shared_ptr<const std::string> text;
    bool empty() const { return text == nullptr || text->empty(); }
    friend bool operator==(const SharedText& a, const SharedText& b) {
        return a.text == b.text || (a.text != nullptr && b.text != nullptr && *a.text == *b.text);
    }
};

/*  The decoder's listening situation: what plays and on which channels, the layout, the listener's two sliders,
 *  the weighting, the tracker. State rather than parameters, since none of it is automated: a bounce hears it whole,
 *  and so does the worker that designs the filters from it. Sparse in the document: written when not the default. */
struct DecoderState {
    bool headphonesPlay{true}, speakersPlay{false};
    int headphoneChannel{1};    ///< the first channel of the pair, counted from 1
    int hrtf{0};                ///< 0 the model; 1 the KU100 as measured; 2 a SOFA file, fitted on load
    std::string hrtfFile;       ///< the SOFA file's path, for the page
    SharedText hrtfFileHead;    ///< the file's head as a head file's text, so a session opens without the file
    float headSize{1.0f};       ///< the head slider, 0.75..1.3
    float earShift{0.0f};       ///< the ear slider, ERB, -2..2
    bool throughLayout{false};  ///< headphones hear the layout's loudspeakers as points
    int weighting{0};           ///< 0 one listener (basic), 1 audience (max-rE)
    bool distanceCompensation{true};
    std::string layoutName;           ///< a built-in's name, or the file's, for the page
    std::vector<Loudspeaker> layout;  ///< the loudspeakers themselves, so a session opens without the file
    int trackerSource{0};             ///< 0 none, 1 OSC, 2 AirPods through the bundled helper; a phone later
    bool trackerOverrides{true};      ///< the tracker replaces yaw, pitch and roll while it sends
    int oscPort{9000};
    bool oscNetwork{false};                         ///< bound to this computer until asked
    std::array<bool, 3> flip{false, false, false};  ///< learn mode's signs
    /// How the AirPods sit on the head, as look-and-nod found it: rows the head's front, left and up in the
    /// sensor's frame. The frame they report worn upright (x right, y forward, z up) until a calibration replaces it.
    std::array<double, 9> airpodsAxes{0, 1, 0, -1, 0, 0, 0, 0, 1};
    bool airpodsCalibrated{false};

    friend bool operator==(const DecoderState&, const DecoderState&) = default;
};

struct RoomState {
    int preset{3};  ///< hall: what a fresh Reverb's parameters are
    int shape{1};   ///< RoomShape::Hall

    friend bool operator==(const RoomState&, const RoomState&) = default;
};

struct PresetRef {
    bool factory{false};
    std::string folder;  ///< empty: loose under its group
    std::string name;    ///< empty: no preset at all

    bool none() const { return name.empty(); }
    friend bool operator==(const PresetRef&, const PresetRef&) = default;
};

/// What a window may be resized to, as a multiple of its design size.
inline constexpr float kWindowScaleMin = 0.5f, kWindowScaleMax = 2.0f;

struct PluginState {
    /// One state type shared by every plugin; only the trajectory is encoder-only, written when
    /// not default so an effect's document carries none.
    Identity identity;
    PresetRef preset;
    TrajectoryState trajectory;
    std::vector<MatrixCell> matrix;
    std::array<SourceSlot, kNumSources> sources{};
    std::array<EnvTrigger, 3> envTriggers{};
    std::array<RegionEntry, kMaxRegions> regions{};
    /// What a bounce should do. Written only when not the default, so a plugin with no quality
    /// switch carries nothing.
    RenderQuality renderQuality{RenderQuality::Realistic};
    RoomState room;        ///< Reverb's; sparse
    int echoPattern{2};    ///< Echo's: spiral, what a fresh Echo's taps are; sparse
    DecoderState decoder;  ///< the decoder's; sparse
    /// Window size as a multiple of its design size. Belongs to the session, not the sound: no
    /// preset carries it, a preset load keeps it, undo doesn't touch it. Sparse.
    float windowScale{1.0f};
    std::array<float, kMaxParams> params{};  ///< sized to the shared cap, not this plugin's count

    PluginState() = default;  ///< parameters at zero: it does not know whose state it is
    /// Every parameter to its default, per `m`, the manifest this state belongs to.
    void resetParamsToDefaults(const ParamManifest& m);

    /// A state belonging to a plugin: its parameters start at that plugin's defaults.
    explicit PluginState(const ParamManifest& m);
};

struct LoadResult {
    bool ok{false};
    int fromVersion{0};
    bool fromFuture{false};  ///< written by a newer build; loaded best-effort
    /// The document was another plugin's. Nothing is loaded: a shared key would land and the
    /// writer's own would not, leaving a half-applied patch nobody asked for.
    bool fromWrongProduct{false};
    Product product{Product::Unknown};
    std::string message;
};

/// Serialisation reads the manifest, not a plugin's enum: a parameter is written under its key
/// and read back by looking that key up, so each of these takes the manifest of the plugin whose
/// document it is.
Json toJson(Product p, const ParamManifest& m, const PluginState& s);
LoadResult fromJson(Product p, const ParamManifest& m, const Json& j, PluginState& out);

/// Convenience: serialise/parse a whole document.
std::string saveState(Product p, const ParamManifest& m, const PluginState& s);
LoadResult loadState(Product p, const ParamManifest& m, std::string_view text, PluginState& out);

// Enum -> string. Deliberately NOT called toString: doctest treats a free toString in the
// type's namespace as its stringification hook, and ours returns string_view.
// Parsing an unknown name yields the default rather than failing, so a document from a
// newer build that gained a generator still opens.
std::string_view name(TrajectoryKind v);
std::string_view name(GeneratorType v);
std::string_view name(SourceInput v);
std::string_view name(MatrixTab v);
std::string_view name(TriggerInput v);
std::string_view name(TriggerGate v);
std::string_view name(RegionKind v);
std::string_view name(RegionSide v);
std::string_view name(RenderQuality v);

/// Parameter names for a generator, in order. Serialised by NAME, never by index.
std::span<const std::string_view> generatorParamNames(GeneratorType g);

}  // namespace bambi
