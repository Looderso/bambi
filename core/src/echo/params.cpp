// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/echo/params.hpp"

#include <array>
#include <cstdint>
#include <iterator>
#include <string_view>

namespace bambi {
namespace {

constexpr std::array<ParamDesc, kNumEchoParams> kTable{{
#define BAMBI_X(grp, id_, key_, name_, unit_, type_, mn, mx, df, ch) \
    ParamDesc{EchoParam::id_,                                        \
              ParamGroup::grp,                                       \
              key_,                                                  \
              name_,                                                 \
              unit_,                                                 \
              ParamType::type_,                                      \
              static_cast<float>(mn),                                \
              static_cast<float>(mx),                                \
              static_cast<float>(df),                                \
              ch == nullptr ? std::string_view{} : std::string_view{ch}},
    BAMBI_ECHO_PARAM_LIST(BAMBI_X)
#undef BAMBI_X
}};

//  Mid-travel values for knobs whose useful range is mostly at the low end.
constexpr float skewOf(std::string_view k) {
    if (const float shared = sharedSkew(k); shared > 0.0f) return shared;
    if (k.ends_with(".ms") || k.ends_with(".offset_ms")) return 250.0f;
    if (k.ends_with(".blur")) return 30.0f;
    if (k.ends_with(".low_cut")) return 200.0f;
    if (k.ends_with(".high_cut")) return 3000.0f;
    return 0.0f;
}

constexpr float kSkewTable[] = {
#define BAMBI_X(grp, id_, key_, name_, unit_, type_, mn, mx, df, ch) skewOf(key_),
    BAMBI_ECHO_PARAM_LIST(BAMBI_X)
#undef BAMBI_X
};

//  Modulation targets. A tap's switches and step counts are not: they have no in-between values.
constexpr DestKind destOf(std::string_view k) {
    if (const DestKind shared = sharedDestKind(k); shared != DestKind::NotModulatable) return shared;
    if (k == "output.wet" || k == "output.dry") return DestKind::DirectScalar;
    //  Prefix too: `.level` alone would also match `mod.amount.level`.
    if (!k.starts_with("tap")) return DestKind::NotModulatable;
    if (k.ends_with(".spin")) return DestKind::Rate;
    if (k.ends_with(".az") || k.ends_with(".el")) return DestKind::DirectAngle;
    if (k.ends_with(".level") || k.ends_with(".feedback") || k.ends_with(".skew") || k.ends_with(".blur") ||
        k.ends_with(".low_cut") || k.ends_with(".high_cut"))
        return DestKind::DirectScalar;
    return DestKind::NotModulatable;
}

constexpr DestKind kDestTable[] = {
#define BAMBI_X(grp, id_, key_, name_, unit_, type_, mn, mx, df, ch) destOf(key_),
    BAMBI_ECHO_PARAM_LIST(BAMBI_X)
#undef BAMBI_X
};

constexpr double smoothingOfKey(std::string_view k) {
    switch (destOf(k)) {
        case DestKind::Rate: return 0.020;
        case DestKind::DirectScalar: return 0.040;
        case DestKind::DirectAngle: return 720.0;  // degrees a second
        case DestKind::NotModulatable: break;
    }
    return 0.0;
}

constexpr double kSmoothingTable[] = {
#define BAMBI_X(grp, id_, key_, name_, unit_, type_, mn, mx, df, ch) smoothingOfKey(key_),
    BAMBI_ECHO_PARAM_LIST(BAMBI_X)
#undef BAMBI_X
};

//  What wraps around its range.
constexpr bool wraps(std::string_view k) {
    return sharedWraps(k) || (k.starts_with("tap") && (k.ends_with(".az") || k.ends_with(".spin")));
}

constexpr std::uint8_t kWrapsTable[] = {
#define BAMBI_X(grp, id_, key_, name_, unit_, type_, mn, mx, df, ch) static_cast<std::uint8_t>(wraps(key_)),
    BAMBI_ECHO_PARAM_LIST(BAMBI_X)
#undef BAMBI_X
};

static_assert(std::size(kSkewTable) == kNumEchoParams);
static_assert(std::size(kWrapsTable) == kNumEchoParams);
static_assert(std::size(kDestTable) == kNumEchoParams);
static_assert(std::size(kSmoothingTable) == kNumEchoParams);
static_assert(kNumEchoParams <= kMaxParams, "Echo's list has outgrown the cap the bus and the engines share");

}  // namespace

const ParamManifest& echoParams() {
    static const ParamManifest m{kTable, kSkewTable, kDestTable, kSmoothingTable, kWrapsTable};
    return m;
}

const ModManifest& echoMod() {
    //  The row always shown: the one control heard whichever taps are on.
    static const char* const kBase[] = {"output.wet"};
    static const ModManifestOwner built(echoParams(), kBase);
    return built.get();
}

}  // namespace bambi
