// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace bambi {

/*  A parameter list is a plugin's own; this file holds the shape of a manifest and the machinery
 *  that reads one, so shared code works over any plugin's list without naming it.
 *
 *  Hosts persist automation against the parameter key, so once released a key can never be
 *  renamed or removed, only appended to. Display names, ranges and defaults may still change.
 *  Enforced by test_parameters.cpp against a frozen golden list.
 */
enum class ParamType : std::uint8_t { Float, Choice, Bool };

/// How a parameter answers modulation. `NotModulatable` is a parameter no cell can reach.
enum class DestKind : std::uint8_t { NotModulatable, Rate, DirectScalar, DirectAngle };

enum class ParamGroup : std::uint8_t { Transform, Motion, Render, Input, Modulation, Lfo, Envelope, Region };

/// The largest parameter list any plugin may have; shared machinery sizes its fixed arrays by
/// this, including the link bus's `kLinkMaxParams`.
inline constexpr int kMaxParams = 192;

/// The most region slots any plugin has. `ParamManifest::regionCount()` says how many a given
/// plugin actually has.
inline constexpr int kMaxRegions = 4;

/// No parameter: a sentinel outside every manifest, so it cannot collide with a valid index.
inline constexpr int kNoParam = -1;

/// A parameter's position in its plugin's list. Opaque: only a plugin names them
/// (BAMBI_DEFINE_PARAM_IDS below).
enum class ParamId : std::uint16_t;

/// No parameter, as a ParamId. 0xFFFF is past every list kMaxParams allows.
inline constexpr ParamId kNoParamId = static_cast<ParamId>(0xFFFF);

/// A plugin's parameter handles, from its X-macro list: `NS::Name` is the ParamId of that entry,
/// numbered in list order, and `NS::kCount` is how many there are.
#define BAMBI_PARAM_INDEX_OF(grp, id, key, name, unit, type, mn, mx, df, ch) id,
#define BAMBI_PARAM_HANDLE_OF(grp, id, key, name, unit, type, mn, mx, df, ch) \
    inline constexpr ::bambi::ParamId id = static_cast<::bambi::ParamId>(index::id);
#define BAMBI_DEFINE_PARAM_IDS(NS, LIST)                       \
    namespace NS {                                             \
    namespace index {                                          \
    enum : std::uint16_t { LIST(BAMBI_PARAM_INDEX_OF) Count }; \
    }                                                          \
    LIST(BAMBI_PARAM_HANDLE_OF)                                \
    inline constexpr int kCount = index::Count;                \
    }

struct ParamDesc {
    ParamId id;
    ParamGroup group;
    std::string_view key;  ///< stable; never rename, never remove
    std::string_view name;
    std::string_view unit;
    ParamType type;
    float min, max, def;
    std::string_view choices;  ///< comma-separated for Choice, empty otherwise
};

/// A parameter's range and default, in the units of its control, for code that clamps or opens
/// without a manifest at hand.
struct ParamSpec {
    double min{0.0};
    double max{0.0};
    double def{0.0};
};

/// Expands one X-macro entry into a match on the key `prefix` + `suffix`.
#define BAMBI_PARAM_SPEC_OF(grp, id, key, name, unit, type, mn, mx, df, ch)                        \
    if (const std::string_view k{key};                                                             \
        k.size() == prefix.size() + suffix.size() && k.starts_with(prefix) && k.ends_with(suffix)) \
        return ::bambi::ParamSpec{static_cast<double>(mn), static_cast<double>(mx), static_cast<double>(df)};

/*  A plugin's compile-time view of its own list, so code that clamps or opens without a manifest
 *  reads the numbers the host is given: `NAME##ParamSpec(prefix, suffix)` gives a key's range and
 *  default (an unknown key fails to compile in a constant expression), `NAME##Default(key)` its
 *  default, always at compile time. */
#define BAMBI_DEFINE_PARAM_SPECS(NAME, LIST)                                                              \
    constexpr ::bambi::ParamSpec NAME##ParamSpec(std::string_view prefix, std::string_view suffix = {}) { \
        LIST(BAMBI_PARAM_SPEC_OF)                                                                         \
        throw #NAME "ParamSpec: no such key";                                                             \
    }                                                                                                     \
    consteval double NAME##Default(std::string_view key) { return NAME##ParamSpec(key).def; }

/// One plugin's list, plus side tables indexed by position for what is true of a parameter but
/// not its range: skew, modulation target, smoothing, wrapping. Empty spans read as "linear" and
/// "not modulatable". A manifest is a view: the tables it names are static in the plugin that
/// owns them.
struct ParamManifest {
    std::span<const ParamDesc> descs;
    std::span<const float> skew;     ///< value at mid-travel by position; 0 or absent is linear
    std::span<const DestKind> dest;  ///< by position; absent is NotModulatable
    std::span<const double>
        smoothing;  ///< by position, seconds (or deg/s for an angle); double so a smoother's time constant keeps its low bits
    /// By position: 1 where the range is one turn of something circular, so a value past one end
    /// wraps in at the other rather than clamping.
    std::span<const std::uint8_t> wraps;

    int size() const { return static_cast<int>(descs.size()); }
    bool has(int at) const { return at >= 0 && at < size(); }
    const ParamDesc& operator[](int at) const { return descs[static_cast<std::size_t>(at)]; }

    /// Look up by stable key. `kNoParam` when absent.
    int byKey(std::string_view key) const;

    /// How many region slots this plugin stamped (`region1.side`, `region2.side`, ...).
    int regionCount() const;

    float skewCentreOf(int at) const;  ///< 0 for a linear range
    DestKind destKindOf(int at) const;
    double smoothingOf(int at) const;  ///< 0 when it is not modulatable
    bool wrapsAt(int at) const;        ///< its range is one turn, and it wraps
};

/// Clamp to the descriptor's range; for Choice/Bool also rounds to an integer step.
float clampToRange(const ParamManifest& m, int at, float value);

/// A host parameter's normalised position, and back; skewed the way JUCE's
/// NormalisableRange::setSkewForCentre would. Not clamped, so a modulated reach past a range's
/// ends can still be drawn.
float toNormalised(const ParamManifest& m, int at, float value);
float fromNormalised(const ParamManifest& m, int at, float normalised);

int choiceCount(const ParamDesc& d);

/// A value as the interface writes it: "90 °/s", "0.0 dB", "0.25 hz", "12 ms", two decimals with
/// no unit, a choice by name, "on"/"off". Negative uses a real minus sign (U+2212); zero is never
/// "-0".
std::string formatParameter(const ParamDesc& d, double value);

/// A number rounded to `decimals`, with a real minus sign and no "-0".
std::string formatNumber(double value, int decimals);

}  // namespace bambi
