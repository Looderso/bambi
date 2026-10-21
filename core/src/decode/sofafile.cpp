// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/decode/sofafile.hpp"

#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>

#include "bambi/dsp/resample.hpp"
#include "bambi/math/sh.hpp"
#include "mysofa.h"

namespace bambi {

namespace {
bool fail(std::string* error, const std::string& what) {
    if (error) *error = what;
    return false;
}

/// Why libmysofa loaded nothing: the file missing, or not a SOFA file.
bool unloaded(std::string* error, int err) {
    return fail(error, err == ENOENT ? std::string("the file is not there")
                                     : "not a SOFA file (libmysofa error " + std::to_string(err) + ")");
}

/// The loaded structure into a measured head; frees it either way.
bool take(MYSOFA_HRTF* h, int err, MeasuredHead& out, std::string* error) {
    if (h == nullptr) return unloaded(error, err);
    if (const int check = mysofa_check(h); check != MYSOFA_OK) {
        mysofa_free(h);
        return fail(error, "not a SOFA file of impulse responses as AES69 lays them out (libmysofa check " +
                               std::to_string(check) + ")");
    }
    if (h->R != 2) {
        const std::string r = std::to_string(h->R);
        mysofa_free(h);
        return fail(error, "not two receivers, the two ears: " + r);
    }
    const std::size_t M = h->M, N = h->N;
    if (h->DataIR.elements != M * 2 * N || h->SourcePosition.elements != M * 3 || h->DataSamplingRate.elements < 1) {
        mysofa_free(h);
        return fail(error, "the file's arrays do not fit its dimensions");
    }
    mysofa_tocartesian(h);  // source positions to metres, x front, y left, z up
    //  the first receiver is the left ear: the check above refuses a file whose receivers sit the other way
    out.sampleRate = h->DataSamplingRate.values[0];
    out.taps = static_cast<int>(N);
    out.directions.resize(M);
    out.left.resize(M * N);
    out.right.resize(M * N);
    for (std::size_t m = 0; m < M; ++m) {
        const float* p = h->SourcePosition.values + m * 3;
        out.directions[m] = unit(Vec3{p[0], p[1], p[2]});
        const float* ir = h->DataIR.values + m * 2 * N;
        const float* l = ir;
        const float* r = ir + N;
        for (std::size_t n = 0; n < N; ++n) {
            out.left[m * N + n] = l[n];
            out.right[m * N + n] = r[n];
        }
    }
    mysofa_free(h);
    return out.sampleRate > 0 && out.taps > 0 ? true : fail(error, "no sample rate or no samples");
}
}  // namespace

bool readSofa(const std::string& path, MeasuredHead& out, std::string* error) {
    int err = 0;
    MYSOFA_HRTF* h = mysofa_load(path.c_str(), &err);
    if (!take(h, err, out, error)) return false;
    const std::size_t slash = path.find_last_of("/\\");
    out.name = slash == std::string::npos ? path : path.substr(slash + 1);
    return true;
}

bool readSofaData(std::span<const char> bytes, MeasuredHead& out, std::string* error) {
    int err = 0;
    MYSOFA_HRTF* h = mysofa_load_data(bytes.data(), bytes.size(), &err);
    if (!take(h, err, out, error)) return false;
    out.name = "a SOFA file";
    return true;
}

namespace {
std::string attribute(MYSOFA_HRTF* h, const char* name) {
    char key[64];
    std::snprintf(key, sizeof key, "%s", name);
    const char* v = mysofa_getAttribute(h->attributes, key);
    return v != nullptr ? std::string(v) : std::string();
}
}  // namespace

bool readSofaMatrix(const std::string& path, FirMatrix& out, std::string* error) {
    int err = 0;
    MYSOFA_HRTF* h = mysofa_load(path.c_str(), &err);
    if (h == nullptr) return unloaded(error, err);
    const std::string type = attribute(h, "DataType");
    if (type != "FIR-E") {
        mysofa_free(h);
        return fail(error, "not a FIR-E file: its DataType is \"" + type + "\"");
    }
    const std::size_t M = h->M, R = h->R, N = h->N, E = h->E;
    if (M != 1 || R < 1 || E < 1 || N < 1 || h->DataIR.elements != M * R * N * E || h->DataSamplingRate.elements < 1) {
        mysofa_free(h);
        return fail(error, "the file's arrays do not fit one measurement of receivers x taps x emitters");
    }
    out.sampleRate = h->DataSamplingRate.values[0];
    out.inputs = static_cast<int>(E);
    out.outputs = static_cast<int>(R);
    out.taps = static_cast<int>(N);
    out.filters.resize(E * R * N);
    //  Data.IR is [M R N E]: the emitter runs fastest
    for (std::size_t r = 0; r < R; ++r)
        for (std::size_t n = 0; n < N; ++n)
            for (std::size_t e = 0; e < E; ++e)
                out.filters[(e * R + r) * N + n] = h->DataIR.values[(r * N + n) * E + e];
    const std::string order = attribute(h, "AmbisonicsOrder");
    //  a number, or none: an attribute that does not parse is as good as absent
    out.order = -1;
    if (int read = 0;
        !order.empty() && std::from_chars(order.data(), order.data() + order.size(), read).ec == std::errc{})
        out.order = read;
    out.channelOrder = attribute(h, "AmbisonicsChannelOrder");
    out.normalization = attribute(h, "AmbisonicsNormalization");
    out.title = attribute(h, "Title");
    mysofa_free(h);
    return out.sampleRate > 0 ? true : fail(error, "no sample rate");
}

int resampledFilters(const FirMatrix& m, int fieldChannels, double toHz, bool n3d, std::vector<float>& out, int& taps) {
    const int inputs = std::min(fieldChannels, m.inputs);
    std::vector<float> one, scaled;
    resampleSinc(m.filter(0, 0), m.sampleRate, toHz, one);
    taps = static_cast<int>(one.size());
    out.assign(
        static_cast<std::size_t>(fieldChannels) * static_cast<std::size_t>(m.outputs) * static_cast<std::size_t>(taps),
        0.0f);
    for (int i = 0; i < inputs; ++i) {
        const float factor = n3d ? static_cast<float>(std::sqrt(2.0 * acnOrder(i) + 1.0)) : 1.0f;
        for (int o = 0; o < m.outputs; ++o) {
            resampleSinc(m.filter(i, o), m.sampleRate, toHz, scaled);
            float* dst = out.data() + (static_cast<std::size_t>(i) * static_cast<std::size_t>(m.outputs) +
                                       static_cast<std::size_t>(o)) *
                                          static_cast<std::size_t>(taps);
            for (std::size_t t = 0; t < scaled.size() && t < static_cast<std::size_t>(taps); ++t)
                dst[t] = scaled[t] * factor;
        }
    }
    return inputs;
}

}  // namespace bambi
