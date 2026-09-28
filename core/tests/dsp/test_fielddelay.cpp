// SPDX-License-Identifier: GPL-3.0-or-later
#include <vector>

#include "bambi/dsp/fielddelay.hpp"
#include "doctest.h"

using namespace bambi;

namespace {

//  Frame g, channel c carries a number that says which it is, so a frame read from the wrong place
//  or the wrong channel is wrong by something that can be read.
float mark(std::int64_t frame, int channel) { return static_cast<float>(frame * 10 + channel + 1); }

void writeFrames(FieldDelayLine& line, std::int64_t first, int frames) {
    std::vector<float> in(static_cast<std::size_t>(frames * line.channels()));
    for (int f = 0; f < frames; ++f)
        for (int c = 0; c < line.channels(); ++c)
            in[static_cast<std::size_t>(f * line.channels() + c)] = mark(first + f, c);
    line.write(in.data(), frames);
}

}  // namespace

TEST_CASE("what was written comes back a delay later, across the wrap and however it was handed over") {
    FieldDelayLine line;
    line.prepare(3, 50);
    std::int64_t total = 0;
    for (const int n : {7, 1, 30, 11, 1, 1, 40, 13, 50, 2}) {  // past the ring's end several times
        writeFrames(line, total, n);
        total += n;
        for (const int back : {1, 5, 37, 50}) {
            const int frames = std::min(back, 9);
            std::vector<float> out(static_cast<std::size_t>(frames * 3), -1.0f);
            line.fetch(back, frames, out.data());
            for (int f = 0; f < frames; ++f)
                for (int c = 0; c < 3; ++c) {
                    const std::int64_t g = total - back + f;
                    REQUIRE(out[static_cast<std::size_t>(f * 3 + c)] == (g >= 0 ? mark(g, c) : 0.0f));
                }
        }
    }
}

TEST_CASE("a restart costs nothing and leaves silence, up to the frame it happened on") {
    FieldDelayLine line;
    line.prepare(2, 64);
    writeFrames(line, 0, 200);  // the ring is full of old frames, and stays so
    line.restart();
    CHECK(line.written() == 0);
    std::vector<float> out(64 * 2, -1.0f);
    line.fetch(64, 64, out.data());
    for (const float v : out) CHECK(v == 0.0f);  // the memory still holds them; they are not read

    //  ten new frames: a read that straddles the restart is silence up to it and the new frames after
    writeFrames(line, 1000, 10);
    line.fetch(16, 16, out.data());
    for (int f = 0; f < 16; ++f)
        for (int c = 0; c < 2; ++c)
            CHECK(out[static_cast<std::size_t>(f * 2 + c)] == (f < 6 ? 0.0f : mark(1000 + f - 6, c)));

    //  farther back than the ring is long, there is nothing, whatever has been written
    writeFrames(line, 2000, 300);
    line.fetch(65, 8, out.data());
    for (int k = 0; k < 16; ++k) CHECK(out[static_cast<std::size_t>(k)] == 0.0f);
}
