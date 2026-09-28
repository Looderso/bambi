// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/encode/params.hpp"

#include <array>
#include <string>
#include <string_view>

#include "bambi/encode/pathparams.hpp"

/*  The encoder's list, and the three tables beside it. The side tables are built from key strings
 *  rather than from enumerators, so they cannot drift when the enum changes -- a misspelling here
 *  is a parameter that quietly stops being modulatable, which is why test_parameters.cpp carries a
 *  frozen list of every key that must be.
 */
namespace bambi {
namespace {

constexpr std::array<ParamDesc, kNumEncoderParams> kTable{{
#define BAMBI_X(grp, id_, key_, name_, unit_, type_, mn, mx, df, ch) \
    ParamDesc{EncoderParam::id_,                                     \
              ParamGroup::grp,                                       \
              key_,                                                  \
              name_,                                                 \
              unit_,                                                 \
              ParamType::type_,                                      \
              static_cast<float>(mn),                                \
              static_cast<float>(mx),                                \
              static_cast<float>(df),                                \
              ch == nullptr ? std::string_view{} : std::string_view{ch}},
    BAMBI_ENCODER_PARAM_LIST(BAMBI_X)
#undef BAMBI_X
}};

/*  The encoder's side tables, by position. A skew where the slow end holds every useful choice, and
 *  how each destination answers modulation -- kept beside the list rather than inside the macro, so
 *  the one thing that can never change is edited as little as possible. */
constexpr float kSkewTable[] = {
#define BAMBI_X(grp, id_, key_, name_, unit_, type_, mn, mx, df, ch) sharedSkew(key_),
    BAMBI_ENCODER_PARAM_LIST(BAMBI_X)
#undef BAMBI_X
};

/// A path setting that is one turn.
constexpr bool pathWrapsOf(ParamId id) {
    const PathKey* p = pathKeyOf(id);
    return p != nullptr && generatorParam(p->shape, p->index).wraps;
}

constexpr DestKind destOf(std::string_view k, ParamId id) {
    if (const DestKind shared = sharedDestKind(k); shared != DestKind::NotModulatable) return shared;
    if (k == "motion.speed" || k == "transform.yaw_rate" || k == "transform.pitch_rate" || k == "transform.roll_rate")
        return DestKind::Rate;
    //  Angles are rate-limited: the complaint about an unsmoothed position jump is always about how
    //  fast it moved, never about how long it took to settle.
    if (k == "motion.displace" || k == "transform.yaw" || k == "transform.pitch" || k == "transform.roll")
        return DestKind::DirectAngle;
    if (k == "transform.extent" || k == "render.width" || k == "render.gain" || k == "input.trim" ||
        k == "input.spread" || k == "input.offset")
        return DestKind::DirectScalar;
    //  a parametric path's settings: those that go round are angles, the rest settle
    if (const PathKey* p = pathKeyOf(id))
        return generatorParam(p->shape, p->index).wraps ? DestKind::DirectAngle : DestKind::DirectScalar;
    return DestKind::NotModulatable;
}

constexpr DestKind kDestTable[] = {
#define BAMBI_X(grp, id_, key_, name_, unit_, type_, mn, mx, df, ch) destOf(key_, EncoderParam::id_),
    BAMBI_ENCODER_PARAM_LIST(BAMBI_X)
#undef BAMBI_X
};

constexpr double smoothingOfKey(std::string_view k, ParamId id) {
    switch (destOf(k, id)) {
        case DestKind::Rate: return 0.020;          // seconds; 90% of target in ~46 ms
        case DestKind::DirectScalar: return 0.040;  // seconds
        case DestKind::DirectAngle:
            //  Degrees per second. Displace is a fraction of a lap rather than an angle, so its limit
            //  is in laps/second and deliberately generous -- it is a position offset, and the
            //  trajectory's own arc length already sets what that costs.
            return k == "motion.displace" ? 4.0 : 720.0;
        case DestKind::NotModulatable: break;
    }
    return 0.0;
}

constexpr double kSmoothingTable[] = {
#define BAMBI_X(grp, id_, key_, name_, unit_, type_, mn, mx, df, ch) smoothingOfKey(key_, EncoderParam::id_),
    BAMBI_ENCODER_PARAM_LIST(BAMBI_X)
#undef BAMBI_X
};

/*  What wraps: the shared blocks', the placement's three angles, and displace -- a place round the
 *  lap, where 1 is 0 again. */
constexpr std::uint8_t kWrapsTable[] = {
#define BAMBI_X(grp, id_, key_, name_, unit_, type_, mn, mx, df, ch)                            \
    static_cast<std::uint8_t>(sharedWraps(key_) || std::string_view(key_) == "transform.yaw" || \
                              std::string_view(key_) == "transform.pitch" ||                    \
                              std::string_view(key_) == "transform.roll" ||                     \
                              std::string_view(key_) == "motion.displace" || pathWrapsOf(EncoderParam::id_)),
    BAMBI_ENCODER_PARAM_LIST(BAMBI_X)
#undef BAMBI_X
};

static_assert(std::size(kSkewTable) == kNumEncoderParams);
static_assert(std::size(kWrapsTable) == kNumEncoderParams);
static_assert(std::size(kDestTable) == kNumEncoderParams);
static_assert(std::size(kSmoothingTable) == kNumEncoderParams);
static_assert(kNumEncoderParams <= kMaxParams, "the manifest has outgrown the cap the bus and the engines share");

}  // namespace

const ParamManifest& encodeParams() {
    static const ParamManifest m{kTable, kSkewTable, kDestTable, kSmoothingTable, kWrapsTable};
    return m;
}

const ModManifest& encodeMod() {
    /*  One row always shown, to show what a row is: speed, the encoder's premise -- loudness moves
        the source. Every other target opens its row when it is touched. Everything else about a
        manifest is the same in every plugin and is built by the shared owner. */
    static const char* const kBase[] = {"motion.speed"};
    static const ModManifestOwner built(encodeParams(), kBase);
    return built.get();
}

}  // namespace bambi
