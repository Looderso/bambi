// SPDX-License-Identifier: GPL-3.0-or-later
//
//  bambi-head: a measured head as a head file the decoder plays.
//
//    bambi-head <measured.sofa | measured.bin> <out.json> [--name "..."] [--order N] [--itd-order N] [--step S]
//               [--roughness L]
//
//  A SOFA file is read as AES69 lays it out. The other measured set is a flat little-endian file: int32 the number of directions, then float32 azimuth and
//  elevation of each in degrees (azimuth counter-clockwise from the front, elevation up), then float32 responses,
//  directions x ears x taps, the left ear first, at 48 kHz; the taps follow from the file's size. It prints how
//  far the fitted model is from the set over 2-16 kHz -- the summed level and the difference in dB, the time
//  difference in microseconds -- and the file's size, so the order and the axis step are chosen by measuring.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "bambi/decode/headmodelfile.hpp"
#include "bambi/decode/measuredhead.hpp"
#include "bambi/decode/sofafile.hpp"
#include "bambi/math/vec3.hpp"

namespace {
bool readMeasured(const std::string& path, bambi::MeasuredHead& m, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "cannot open " + path;
        return false;
    }
    std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (bytes.size() < 4) {
        error = "too short";
        return false;
    }
    std::int32_t count = 0;
    std::memcpy(&count, bytes.data(), 4);
    if (count <= 0) {
        error = "no directions";
        return false;
    }
    const std::size_t n = static_cast<std::size_t>(count);
    const std::size_t header = 4 + n * 2 * 4;
    if (bytes.size() < header || (bytes.size() - header) % (n * 2 * 4) != 0) {
        error = "the size does not fit directions x 2 ears x taps";
        return false;
    }
    m.taps = static_cast<int>((bytes.size() - header) / (n * 2 * 4));
    m.sampleRate = 48000.0;
    m.directions.resize(n);
    std::vector<float> angles(n * 2);
    std::memcpy(angles.data(), bytes.data() + 4, n * 2 * 4);
    for (std::size_t d = 0; d < n; ++d)
        m.directions[d] = bambi::fromAzEl(angles[d * 2] * bambi::kDeg2Rad, angles[d * 2 + 1] * bambi::kDeg2Rad);
    const auto taps = static_cast<std::size_t>(m.taps);
    m.left.resize(n * taps);
    m.right.resize(n * taps);
    const char* at = bytes.data() + header;
    for (std::size_t d = 0; d < n; ++d) {
        std::memcpy(m.left.data() + d * taps, at, taps * 4);
        at += taps * 4;
        std::memcpy(m.right.data() + d * taps, at, taps * 4);
        at += taps * 4;
    }
    return true;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr,
                     "usage: bambi-head <measured.bin> <out.json> [--name N] [--order N] [--itd-order N] [--step S] "
                     "[--roughness L]\n");
        return 2;
    }
    bambi::HeadFit fit;
    std::string name = "a measured head";
    for (int i = 3; i + 1 < argc; i += 2) {
        const std::string key = argv[i], value = argv[i + 1];
        if (key == "--name")
            name = value;
        else if (key == "--order")
            fit.order = std::stoi(value);
        else if (key == "--itd-order")
            fit.itdOrder = std::stoi(value);
        else if (key == "--step") {
            fit.axis.step = std::stod(value);
            fit.axis.points = static_cast<int>((41.6 - fit.axis.erb0) / fit.axis.step) + 1;  // up to about 20 kHz
        } else if (key == "--roughness")
            fit.roughness = std::stod(value);
        else {
            std::fprintf(stderr, "unknown option %s\n", key.c_str());
            return 2;
        }
    }
    bambi::MeasuredHead m;
    std::string error;
    const std::string in = argv[1];
    const bool sofa = in.size() > 5 && in.compare(in.size() - 5, 5, ".sofa") == 0;
    if (sofa ? !bambi::readSofa(in, m, &error) : !readMeasured(in, m, error)) {
        std::fprintf(stderr, "bambi-head: %s\n", error.c_str());
        return 1;
    }
    if (name != "a measured head" || !sofa) m.name = name;
    std::printf("%zu directions, %d taps at %.0f Hz\n", m.directions.size(), m.taps, m.sampleRate);
    bambi::HeadModel model;
    if (!bambi::fitHeadModel(m, fit, model, &error)) {
        std::fprintf(stderr, "bambi-head: %s\n", error.c_str());
        return 1;
    }
    //  measured on a fine axis, so a coarse step is charged for what its interpolation between points costs
    const bambi::HeadModel::Axis fine{fit.axis.erb0, 0.05, static_cast<int>((41.6 - fit.axis.erb0) / 0.05) + 1};
    const bambi::HeadFitError e = bambi::headFitError(m, model, 2000.0, 16000.0, &fine);
    const bambi::HeadFitError low = bambi::headFitError(m, model, 300.0, 2000.0, &fine);
    const std::string json = bambi::headModelJson(model);
    std::printf("order %d, itd order %d, step %.3g (%d points), roughness %g\n", fit.order, fit.itdOrder, fit.axis.step,
                fit.axis.points, fit.roughness);
    std::printf(
        "2-16 kHz: summed level %.2f dB, difference %.2f dB rms; 0.3-2 kHz: %.2f / %.2f dB; "
        "time difference %.1f us rms\n",
        e.cllDb, e.ildDb, low.cllDb, low.ildDb, e.itdUs);
    std::printf("file: %.0f kB\n", static_cast<double>(json.size()) / 1000.0);
    std::ofstream out(argv[2], std::ios::binary);
    out << json;
    return out ? 0 : 1;
}
