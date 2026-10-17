// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/decode/outputs.hpp"

#include <algorithm>
#include <utility>

namespace bambi {

namespace {
const char* outputName(Output which) { return which == Output::Headphones ? "headphones" : "speakers"; }
}  // namespace

std::vector<int> outputChannels(const DecoderState& d, Output which) {
    if (which == Output::Headphones) return {d.headphoneChannel, d.headphoneChannel + 1};
    std::vector<int> out;
    const int shift = std::max(1, d.speakerFirstChannel) - 1;
    if (!d.matrixFile.empty()) {
        for (int c = 1; c <= d.matrixOutputs; ++c) out.push_back(c + shift);
        return out;
    }
    for (const Loudspeaker& s : d.layout)
        if (!s.imaginary) out.push_back(s.channel + shift);
    std::sort(out.begin(), out.end());
    return out;
}

Placement placeOutput(const DecoderState& d, Output which, int bus) {
    Placement p;
    const std::vector<int> mine = outputChannels(d, which);
    p.needs = mine.empty() ? 0 : *std::max_element(mine.begin(), mine.end());
    p.fits = !mine.empty() && p.needs <= bus;
    const Output other = which == Output::Headphones ? Output::Speakers : Output::Headphones;
    const std::vector<int> theirs = outputChannels(d, other);
    for (int c : mine) p.collide = p.collide || std::find(theirs.begin(), theirs.end(), c) != theirs.end();
    p.otherOn = other == Output::Headphones ? d.headphonesPlay : d.speakersPlay;
    const int first = which == Output::Headphones ? d.headphoneChannel : std::max(1, d.speakerFirstChannel);
    p.span = p.needs > 0 ? p.needs - first + 1 : 0;
    const int afterTheirs = theirs.empty() ? 1 : *std::max_element(theirs.begin(), theirs.end()) + 1;
    p.movedTo = which == Output::Headphones ? afterTheirs + (afterTheirs % 2 == 0 ? 1 : 0) : afterTheirs;
    const bool clash = p.fits && p.collide && p.otherOn;
    p.moves = clash && p.movedTo + p.span - 1 <= bus;
    p.stops = clash && !p.moves;
    return p;
}

Lit lightOutput(DecoderState d, Output which, bool on, int bus) {
    const Placement p = placeOutput(d, which, bus);
    Lit lit;
    if (on && !p.fits) {
        lit.notice = p.needs == 0 ? std::string("no layout")
                                  : std::to_string(p.needs) + " channels needed; the bus has " + std::to_string(bus);
        lit.state = std::move(d);
        return lit;
    }
    const Output other = which == Output::Headphones ? Output::Speakers : Output::Headphones;
    const bool moves = on && p.moves, stops = on && p.stops;
    if (moves)
        lit.notice = std::string(outputName(which)) + " moved to " + std::to_string(p.movedTo) + "\xe2\x80\x93" +
                     std::to_string(p.movedTo + p.span - 1);
    else if (stops)
        lit.notice = std::string(outputName(other)) + " stopped: the same channels";
    (which == Output::Headphones ? d.headphonesPlay : d.speakersPlay) = on;
    if (moves) (which == Output::Headphones ? d.headphoneChannel : d.speakerFirstChannel) = p.movedTo;
    if (stops) (other == Output::Headphones ? d.headphonesPlay : d.speakersPlay) = false;
    lit.state = std::move(d);
    return lit;
}

}  // namespace bambi
