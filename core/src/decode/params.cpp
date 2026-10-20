// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/decode/params.hpp"

#include <array>
#include <string_view>

#include "bambi/mod/manifest.hpp"

/// The decoder's list, and the tables beside it. Side tables read key strings, so they cannot drift when
/// the enum changes.
namespace bambi {

namespace {
constexpr std::array<ParamDesc, kNumDecoderParams> kTable{{
#define BAMBI_X(grp, id_, key_, name_, unit_, type_, mn, mx, df, ch) \
    ParamDesc{DecoderParam::id_,                                     \
              ParamGroup::grp,                                       \
              key_,                                                  \
              name_,                                                 \
              unit_,                                                 \
              ParamType::type_,                                      \
              static_cast<float>(mn),                                \
              static_cast<float>(mx),                                \
              static_cast<float>(df),                                \
              ch == nullptr ? std::string_view{} : std::string_view{ch}},
    BAMBI_DECODER_PARAM_LIST(BAMBI_X)
#undef BAMBI_X
}};

/// Nothing of the decoder's own is skewed: angles and a level in decibels are linear as they stand.
constexpr float kSkewTable[] = {
#define BAMBI_X(grp, id_, key_, name_, unit_, type_, mn, mx, df, ch) sharedSkew(key_),
    BAMBI_DECODER_PARAM_LIST(BAMBI_X)
#undef BAMBI_X
};

/// What answers modulation: the scene's three angles and their rates, and the level.
constexpr DestKind destOf(std::string_view k) {
    if (const DestKind shared = sharedDestKind(k); shared != DestKind::NotModulatable) return shared;
    if (k == "head.yaw" || k == "head.pitch" || k == "head.roll") return DestKind::DirectAngle;
    if (k == "output.gain") return DestKind::DirectScalar;
    if (k == "head.yaw_rate" || k == "head.pitch_rate" || k == "head.roll_rate") return DestKind::Rate;
    return DestKind::NotModulatable;
}
constexpr DestKind kDestTable[] = {
#define BAMBI_X(grp, id_, key_, name_, unit_, type_, mn, mx, df, ch) destOf(key_),
    BAMBI_DECODER_PARAM_LIST(BAMBI_X)
#undef BAMBI_X
};

constexpr double smoothingOfKey(std::string_view k) { return defaultSmoothing(destOf(k)); }
constexpr double kSmoothingTable[] = {
#define BAMBI_X(grp, id_, key_, name_, unit_, type_, mn, mx, df, ch) smoothingOfKey(key_),
    BAMBI_DECODER_PARAM_LIST(BAMBI_X)
#undef BAMBI_X
};

/// Yaw and roll come round; pitch stops at straight up and down.
constexpr bool wrapsOf(std::string_view k) { return sharedWraps(k) || k == "head.yaw" || k == "head.roll"; }
constexpr std::uint8_t kWrapsTable[] = {
#define BAMBI_X(grp, id_, key_, name_, unit_, type_, mn, mx, df, ch) static_cast<std::uint8_t>(wrapsOf(key_)),
    BAMBI_DECODER_PARAM_LIST(BAMBI_X)
#undef BAMBI_X
};

static_assert(std::size(kSkewTable) == kNumDecoderParams);
static_assert(std::size(kWrapsTable) == kNumDecoderParams);
static_assert(std::size(kDestTable) == kNumDecoderParams);
static_assert(std::size(kSmoothingTable) == kNumDecoderParams);
static_assert(kNumDecoderParams <= kMaxParams, "the decoder's list has outgrown the cap the bus and the engines share");
}  // namespace

const ParamManifest& decodeParams() {
    static const ParamManifest m{kTable, kSkewTable, kDestTable, kSmoothingTable, kWrapsTable};
    return m;
}

const ModManifest& decodeMod() {
    //  One row always shown, to show what a row is: yaw, the one thing a monitor's modulation would move.
    static const char* const kBase[] = {"head.yaw"};
    static const ModManifestOwner built(decodeParams(), kBase);
    return built.get();
}

}  // namespace bambi
