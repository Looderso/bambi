// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/reverb/params.hpp"

#include <array>
#include <string_view>

#include "bambi/mod/manifest.hpp"

/// Reverb's list, and the tables beside it. Side tables read key strings, so they cannot drift when
/// the enum changes.
namespace bambi {
namespace {

constexpr std::array<ParamDesc, kNumReverbParams> kTable{{
#define BAMBI_X(grp, id_, key_, name_, unit_, type_, mn, mx, df, ch) \
    ParamDesc{ReverbParam::id_,                                      \
              ParamGroup::grp,                                       \
              key_,                                                  \
              name_,                                                 \
              unit_,                                                 \
              ParamType::type_,                                      \
              static_cast<float>(mn),                                \
              static_cast<float>(mx),                                \
              static_cast<float>(df),                                \
              ch == nullptr ? std::string_view{} : std::string_view{ch}},
    BAMBI_REVERB_PARAM_LIST(BAMBI_X)
#undef BAMBI_X
}};

/// A skew holding the low end where most choices sit -- decay, size, distance, pre-delay -- and the
/// input's cuts near the geometric middle of their range.
constexpr float reverbSkew(std::string_view k) {
    if (k == "room.decay") return 1.5f;
    if (k == "room.size") return 12.0f;
    if (k == "room.distance") return 5.0f;
    if (k == "output.pre_delay") return 40.0f;
    if (k == "input.low_cut") return 100.0f;
    if (k == "input.high_cut") return 6000.0f;
    return 0.0f;
}

constexpr float kSkewTable[] = {
#define BAMBI_X(grp, id_, key_, name_, unit_, type_, mn, mx, df, ch) \
    sharedSkew(key_) > 0.0f ? sharedSkew(key_) : reverbSkew(key_),
    BAMBI_REVERB_PARAM_LIST(BAMBI_X)
#undef BAMBI_X
};

/// What answers modulation: the room itself, and where the send and return point. A region's kind and
/// the quality switch do not, since a cell landing between two choices means nothing.
constexpr DestKind destOf(std::string_view k) {
    if (const DestKind shared = sharedDestKind(k); shared != DestKind::NotModulatable) return shared;
    if (k == "room.size" || k == "room.decay" || k == "room.tone" || k == "room.roughness" || k == "room.distance" ||
        k == "output.wet" || k == "output.dry" || k == "output.pre_delay" || k == "input.low_cut" ||
        k == "input.high_cut")
        return DestKind::DirectScalar;
    return DestKind::NotModulatable;
}

constexpr DestKind kDestTable[] = {
#define BAMBI_X(grp, id_, key_, name_, unit_, type_, mn, mx, df, ch) destOf(key_),
    BAMBI_REVERB_PARAM_LIST(BAMBI_X)
#undef BAMBI_X
};

constexpr double smoothingOfKey(std::string_view k) {
    switch (destOf(k)) {
        case DestKind::Rate: return 0.020;
        case DestKind::DirectScalar:
            // Room size rebuilds and crossfades the delay network when it moves far enough, so it is
            // smoothed harder than a gain: a size that jitters would fade forever.
            return k == "room.size" ? 0.250 : 0.040;
        case DestKind::DirectAngle: return 720.0;  // degrees a second
        case DestKind::NotModulatable: break;
    }
    return 0.0;
}

constexpr double kSmoothingTable[] = {
#define BAMBI_X(grp, id_, key_, name_, unit_, type_, mn, mx, df, ch) smoothingOfKey(key_),
    BAMBI_REVERB_PARAM_LIST(BAMBI_X)
#undef BAMBI_X
};

// What wraps: the shared blocks' alone.
constexpr std::uint8_t kWrapsTable[] = {
#define BAMBI_X(grp, id_, key_, name_, unit_, type_, mn, mx, df, ch) static_cast<std::uint8_t>(sharedWraps(key_)),
    BAMBI_REVERB_PARAM_LIST(BAMBI_X)
#undef BAMBI_X
};

static_assert(std::size(kSkewTable) == kNumReverbParams);
static_assert(std::size(kWrapsTable) == kNumReverbParams);
static_assert(std::size(kDestTable) == kNumReverbParams);
static_assert(std::size(kSmoothingTable) == kNumReverbParams);
static_assert(kNumReverbParams <= kMaxParams, "Reverb's list has outgrown the cap the bus and the engines share");

}  // namespace

const ParamManifest& reverbParams() {
    static const ParamManifest m{kTable, kSkewTable, kDestTable, kSmoothingTable, kWrapsTable};
    return m;
}

const ModManifest& reverbMod() {
    // One row always shown, to demonstrate the shape: distance, which changes gains and nothing
    // else -- not room size, whose every move fades one network into the next.
    static const char* const kBase[] = {"room.distance"};
    static const ModManifestOwner built(reverbParams(), kBase);
    return built.get();
}

}  // namespace bambi
