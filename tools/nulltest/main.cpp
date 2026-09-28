// SPDX-License-Identifier: GPL-3.0-or-later
//
//  bambi-nulltest — are two renders the same, sample for sample?
//
//  The hardest host-contract check is "offline bounce identical to realtime, twice". A null
//  test by ear, or by inverting one track against another in the DAW, cannot tell identical
//  from nearly identical — and nearly identical is exactly the bug worth finding: a phase
//  integrator that drifts by one control step sounds fine and is not deterministic.
//
//  So this compares bits. When files differ it first looks for the one benign explanation, a
//  constant offset from pre-roll or latency, before calling them different.
//
//  exit 0  identical (or within --tolerance)
//  exit 1  different
//  exit 2  not comparable: channel count or sample rate differ, or a file will not read
//  exit 3  identical after a constant shift — a timing offset, not a determinism failure

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "bambi/io/wav.hpp"

using namespace bambi;

namespace {

double toDb(double v) { return v > 0.0 ? 20.0 * std::log10(v) : -1e9; }

/*  Does b, shifted by `offset` frames, equal a exactly wherever they overlap? a[i] against
 *  b[i + offset]. Requires a real overlap, so two files that merely share a silent stretch
 *  cannot "match" at an arbitrary offset. */
bool identicalAtOffset(const AudioBuffer& a, const AudioBuffer& b, long offset) {
    const long na = static_cast<long>(a.numFrames()), nb = static_cast<long>(b.numFrames());
    const long start = std::max(0L, -offset);
    const long end = std::min(na, nb - offset);
    if (end - start < 4800) return false;  // less than 0.1 s at 48 kHz proves nothing
    bool sawSignal = false;
    for (std::size_t c = 0; c < a.channels.size(); ++c) {
        const auto& ca = a.channels[c];
        const auto& cb = b.channels[c];
        for (long i = start; i < end; ++i) {
            const float x = ca[static_cast<std::size_t>(i)];
            if (x != cb[static_cast<std::size_t>(i + offset)]) return false;
            if (x != 0.0f) sawSignal = true;
        }
    }
    return sawSignal;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::printf(
            "bambi-nulltest <a.wav> <b.wav> [--tolerance DB] [--max-shift FRAMES]\n\n"
            "  Compares two renders sample for sample. Exit 0 identical, 1 different,\n"
            "  2 not comparable, 3 identical after a constant shift.\n");
        return 2;
    }
    double toleranceDb = -1e9;
    long maxShift = 16384;
    bool haveTolerance = false;
    for (int i = 3; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--tolerance") == 0) {
            toleranceDb = std::atof(argv[i + 1]);
            haveTolerance = true;
        }
        if (std::strcmp(argv[i], "--max-shift") == 0) maxShift = std::atol(argv[i + 1]);
    }

    AudioBuffer a, b;
    std::string err;
    if (!readWav(argv[1], a, &err)) {
        std::fprintf(stderr, "%s: %s\n", argv[1], err.c_str());
        return 2;
    }
    if (!readWav(argv[2], b, &err)) {
        std::fprintf(stderr, "%s: %s\n", argv[2], err.c_str());
        return 2;
    }

    const auto describe = [](const char* tag, const char* path, const AudioBuffer& x) {
        std::printf("%s  %s\n    %zu ch, %zu frames, %d Hz, %.3f s\n", tag, path, x.channels.size(), x.numFrames(),
                    x.sampleRate, static_cast<double>(x.numFrames()) / std::max(1, x.sampleRate));
    };
    describe("a", argv[1], a);
    describe("b", argv[2], b);

    if (a.channels.size() != b.channels.size() || a.sampleRate != b.sampleRate) {
        std::printf("\nNOT COMPARABLE: channel count or sample rate differ\n");
        return 2;
    }

    const std::size_t n = std::min(a.numFrames(), b.numFrames());
    struct ChannelDiff {
        std::size_t channel;
        double maxDiff;
        long firstFrame;
    };
    std::vector<ChannelDiff> diffs;
    double worst = 0.0;
    for (std::size_t c = 0; c < a.channels.size(); ++c) {
        ChannelDiff d{c, 0.0, -1};
        for (std::size_t i = 0; i < n; ++i) {
            const double diff = std::abs(static_cast<double>(a.channels[c][i]) - b.channels[c][i]);
            if (diff > 0.0 && d.firstFrame < 0) d.firstFrame = static_cast<long>(i);
            d.maxDiff = std::max(d.maxDiff, diff);
        }
        worst = std::max(worst, d.maxDiff);
        diffs.push_back(d);
    }

    if (worst == 0.0) {
        if (a.numFrames() == b.numFrames()) {
            std::printf("\nIDENTICAL — every sample of every channel\n");
        } else {
            std::printf(
                "\nIDENTICAL over the first %zu frames; the lengths differ (%zu vs %zu), "
                "which is a render range, not the audio\n",
                n, a.numFrames(), b.numFrames());
        }
        return 0;
    }

    std::sort(diffs.begin(), diffs.end(),
              [](const ChannelDiff& x, const ChannelDiff& y) { return x.maxDiff > y.maxDiff; });
    std::printf("\nsamples differ — worst channels:\n");
    for (std::size_t k = 0; k < std::min<std::size_t>(8, diffs.size()); ++k) {
        const auto& d = diffs[k];
        if (d.maxDiff == 0.0) break;
        std::printf("    ch %3zu   max diff %8.1f dBFS   first at frame %ld (%.4f s)\n", d.channel, toDb(d.maxDiff),
                    d.firstFrame, static_cast<double>(d.firstFrame) / std::max(1, a.sampleRate));
    }

    //  Before calling it different, the benign explanation: the same audio, shifted.
    for (long s = 1; s <= maxShift; ++s) {
        for (long offset : {s, -s}) {
            if (identicalAtOffset(a, b, offset)) {
                std::printf(
                    "\nIDENTICAL AFTER A SHIFT of %ld frames (%.2f ms): b[i + %ld] == a[i] "
                    "everywhere they overlap.\nA constant timing offset — pre-roll or "
                    "latency — not a determinism failure.\n",
                    offset, 1000.0 * static_cast<double>(offset) / std::max(1, a.sampleRate), offset);
                return 3;
            }
        }
    }

    if (haveTolerance && toDb(worst) <= toleranceDb) {
        std::printf("\nNULL WITHIN TOLERANCE: worst difference %.1f dBFS <= %.1f dBFS\n", toDb(worst), toleranceDb);
        return 0;
    }
    std::printf("\nDIFFERENT: worst difference %.1f dBFS, and no shift within ±%ld frames explains it\n", toDb(worst),
                maxShift);
    return 1;
}
