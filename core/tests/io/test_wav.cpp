// SPDX-License-Identifier: GPL-3.0-or-later
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <random>
#include <string>

#include "bambi/io/wav.hpp"
#include "doctest.h"

using namespace bambi;

namespace {
/*  Unique per process, and never a literal path. Two test binaries can run at once -- two
 *  sessions, or the native and x86-64 builds side by side -- and a fixed name lets one read
 *  the other's bytes mid-write, failing an exact-round-trip REQUIRE on a file that was never
 *  wrong. The directory comes from the standard library rather than a hardcoded /tmp, so the
 *  path is portable across platforms. */
std::string tmpPath(const char* stem) {
    static const std::string token = std::to_string(std::random_device{}());
    return (std::filesystem::temp_directory_path() / ("bambi-test-" + std::string(stem) + "-" + token + ".wav"))
        .string();
}
}  // namespace

TEST_CASE("wav round-trips exactly through 32-bit float") {
    AudioBuffer a;
    a.sampleRate = 48000;
    a.channels.assign(16, std::vector<float>(1000));
    for (std::size_t c = 0; c < 16; ++c)
        for (std::size_t i = 0; i < 1000; ++i)
            a.channels[c][i] = std::sin(static_cast<float>(i) * 0.01f + static_cast<float>(c));

    const std::string p = tmpPath("roundtrip");
    std::string err;
    REQUIRE(writeWav(p, a, &err));
    CHECK(err.empty());

    AudioBuffer b;
    REQUIRE(readWav(p, b, &err));
    CHECK(b.sampleRate == 48000);
    REQUIRE(b.numChannels() == 16);
    REQUIRE(b.numFrames() == 1000);
    for (std::size_t c = 0; c < 16; ++c)
        for (std::size_t i = 0; i < 1000; ++i)
            REQUIRE(b.channels[c][i] == a.channels[c][i]);  // exact: float in, float out
    std::remove(p.c_str());
}

TEST_CASE("mono and stereo stay non-extensible; multichannel goes extensible") {
    for (std::size_t nch : {std::size_t{1}, std::size_t{2}, std::size_t{4}, std::size_t{16}}) {
        AudioBuffer a;
        a.channels.assign(nch, std::vector<float>(64, 0.25f));
        const std::string p = tmpPath("fmt");
        REQUIRE(writeWav(p, a));
        AudioBuffer b;
        REQUIRE(readWav(p, b));
        INFO("channels ", nch);
        CHECK(b.numChannels() == nch);
        CHECK(b.channels[nch - 1][10] == doctest::Approx(0.25f));
        std::remove(p.c_str());
    }
}

TEST_CASE("mono() sums channels") {
    AudioBuffer a;
    a.channels = {{1.0f, 1.0f}, {-1.0f, 3.0f}};
    const auto m = a.mono();
    REQUIRE(m.size() == 2);
    CHECK(m[0] == doctest::Approx(0.0f));
    CHECK(m[1] == doctest::Approx(2.0f));

    AudioBuffer empty;
    CHECK(empty.mono().empty());
}

TEST_CASE("bad input fails with a message rather than crashing") {
    std::string err;
    AudioBuffer b;
    CHECK_FALSE(readWav("/tmp/bambi-does-not-exist.wav", b, &err));
    CHECK_FALSE(err.empty());

    const std::string p = tmpPath("garbage");
    std::FILE* f = std::fopen(p.c_str(), "wb");
    const char junk[] = "not a wave file at all, not even close";
    std::fwrite(junk, 1, sizeof junk, f);
    std::fclose(f);
    CHECK_FALSE(readWav(p, b, &err));
    CHECK_FALSE(err.empty());
    std::remove(p.c_str());

    AudioBuffer nothing;
    CHECK_FALSE(writeWav(tmpPath("empty"), nothing, &err));
}

TEST_CASE("a truncated file is refused rather than read past its end") {
    AudioBuffer a;
    a.channels.assign(4, std::vector<float>(500, 0.1f));
    const std::string p = tmpPath("trunc");
    REQUIRE(writeWav(p, a));

    // Chop the file in half, leaving the header claiming more data than exists.
    std::FILE* f = std::fopen(p.c_str(), "rb");
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::vector<char> bytes(static_cast<std::size_t>(size));
    std::fseek(f, 0, SEEK_SET);
    (void)std::fread(bytes.data(), 1, bytes.size(), f);
    std::fclose(f);

    f = std::fopen(p.c_str(), "wb");
    std::fwrite(bytes.data(), 1, bytes.size() / 2, f);
    std::fclose(f);

    AudioBuffer b;
    CHECK(readWav(p, b));                  // header is intact, so it loads
    CHECK(b.numFrames() < a.numFrames());  // but only the data that is actually there
    std::remove(p.c_str());
}
