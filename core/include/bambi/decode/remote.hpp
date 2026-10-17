// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "bambi/decode/outputs.hpp"
#include "bambi/patch/json.hpp"
#include "bambi/patch/state.hpp"

/*  The Decoder, remote-controlled: what it tells a host that plays its outputs -- the live host's audio sheet --
 *  and what such a host may ask of it. The host only asks: the Decoder grants a request by the rules its own page
 *  follows and reports what it did, so an output lands where the page would have put it. JSON between them, as
 *  `toJson` writes and `reportFrom`/`requestFrom` read it; a field a reader does not know is skipped, one it misses
 *  takes its default. Channels are counted from 1.
 */
namespace bambi {

/// The payload's name, and its version: a change a reader of the last version would misread gets the next number.
inline constexpr const char* kDecoderRemoteKind = "decoder";
constexpr int kDecoderRemoteVersion = 1;

/// What the Decoder tells a host.
struct DecoderReport {
    int bus{0};  ///< its output bus's channels
    bool headphonesOn{false};
    int headphoneChannel{1};  ///< the pair's first
    bool speakersOn{false};
    int speakerFirstChannel{1};        ///< the bus channel the layout's channel 1 plays on
    std::string layout;                ///< a built-in's name; empty for any other layout or none
    std::string layoutName;            ///< what the page calls the layout, built in or not; empty for none
    std::vector<int> speakerChannels;  ///< the bus channels the layout writes, ascending
    std::vector<std::string> layouts;  ///< the built-ins a request may name
    double levelDb{0.0};
    std::string notice;  ///< what the last request or lighting had to say, or empty

    friend bool operator==(const DecoderReport&, const DecoderReport&) = default;
};
DecoderReport reportOf(const DecoderState& d, int bus, double levelDb, std::string notice);
Json toJson(const DecoderReport& r);
DecoderReport reportFrom(const Json& j);

/// What a host may ask; each field left empty is left as it is.
struct DecoderRequest {
    std::optional<bool> headphonesOn, speakersOn;
    std::optional<int> headphoneChannel, speakerFirstChannel;
    std::optional<std::string> layout;  ///< a built-in's name
    std::optional<double> levelDb;      ///< the Decoder's level parameter, set by the Decoder

    friend bool operator==(const DecoderRequest&, const DecoderRequest&) = default;
};
Json toJson(const DecoderRequest& r);
DecoderRequest requestFrom(const Json& j);

/// A state after a request, but for the level: the layout, then the channels, each only where the bus holds it,
/// then each output lit or stopped by `lightOutput`. The notice says what was refused or moved.
Lit grant(DecoderState d, const DecoderRequest& r, int bus);

}  // namespace bambi
