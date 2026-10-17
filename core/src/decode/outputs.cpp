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

namespace {

//  Whether `channels` take a kept channel, run past the bus, or play on two devices.
bool blockedOn(const std::vector<int>& channels, const OutputBus& bus) {
    if (channels.empty()) return false;
    const auto deviceOf = [&](int c) {
        return static_cast<int>(std::lower_bound(bus.deviceEnds.begin(), bus.deviceEnds.end(), c) -
                                bus.deviceEnds.begin());
    };
    for (const int c : channels)
        if (c < 1 || c > bus.channels || std::find(bus.kept.begin(), bus.kept.end(), c) != bus.kept.end() ||
            deviceOf(c) != deviceOf(channels.front()))
            return true;
    return false;
}

std::vector<int> shifted(const std::vector<int>& channels, int by) {
    std::vector<int> out = channels;
    for (int& c : out) c += by;
    return out;
}

bool meets(const std::vector<int>& a, const std::vector<int>& b) {
    for (const int c : a)
        if (std::find(b.begin(), b.end(), c) != b.end()) return true;
    return false;
}

}  // namespace

Placement placeOutput(const DecoderState& d, Output which, const OutputBus& bus) {
    Placement p;
    const std::vector<int> mine = outputChannels(d, which);
    p.needs = mine.empty() ? 0 : *std::max_element(mine.begin(), mine.end());
    p.fits = !mine.empty() && p.needs <= bus.channels;
    const Output other = which == Output::Headphones ? Output::Speakers : Output::Headphones;
    const std::vector<int> theirs = outputChannels(d, other);
    p.collide = meets(mine, theirs);
    p.otherOn = other == Output::Headphones ? d.headphonesPlay : d.speakersPlay;
    const int first = which == Output::Headphones ? d.headphoneChannel : std::max(1, d.speakerFirstChannel);
    p.span = p.needs > 0 ? p.needs - first + 1 : 0;
    p.blocked = p.fits && blockedOn(mine, bus);
    const bool onTheirs = p.fits && p.collide && p.otherOn;
    if (!onTheirs && !p.blocked) return p;

    //  the nearest start with room: past the other's channels when it lands on them, else past its own first,
    //  then from channel 1
    const int afterTheirs = theirs.empty() ? 1 : *std::max_element(theirs.begin(), theirs.end()) + 1;
    const int from = onTheirs ? afterTheirs : first;
    const int step = which == Output::Headphones ? 2 : 1;
    const auto snap = [&](int s) { return which == Output::Headphones && s % 2 == 0 ? s + 1 : s; };
    const auto room = [&](int s) {
        const std::vector<int> there = shifted(mine, s - first);
        return !blockedOn(there, bus) && !(p.otherOn && meets(there, theirs));
    };
    for (int s = snap(from); s + p.span - 1 <= bus.channels && !p.moves; s += step)
        if (s != first && room(s)) {
            p.moves = true;
            p.movedTo = s;
        }
    for (int s = 1; s < from && !p.moves; s += step)
        if (s != first && room(s)) {
            p.moves = true;
            p.movedTo = s;
        }
    if (!p.moves) {
        p.movedTo = snap(afterTheirs);
        p.stops = onTheirs && !p.blocked;
        p.noRoom = p.blocked;
    }
    return p;
}

Lit lightOutput(DecoderState d, Output which, bool on, const OutputBus& bus) {
    const Placement p = placeOutput(d, which, bus);
    Lit lit;
    if (on && (!p.fits || p.noRoom)) {
        lit.notice =
            p.needs == 0 ? std::string("no layout")
            : !p.fits
                ? std::to_string(p.needs) + " channels needed; the bus has " + std::to_string(bus.channels)
                : std::string("no room for the ") + outputName(which) + " clear of the host's channels, on one device";
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
