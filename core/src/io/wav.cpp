// SPDX-License-Identifier: GPL-3.0-or-later
//
//  Backed by dr_wav (public domain / MIT-0): a well-tested container parser, rather than a
//  hand-rolled chunk walk that risks silently mishandling an exactly-consumed chunk.

#include "bambi/io/wav.hpp"

#define DR_WAV_IMPLEMENTATION
#define DR_WAV_NO_STDIO_FLOAT64
#include "dr_wav.h"

namespace bambi {
namespace {
bool fail(std::string* e, const char* msg) {
    if (e) *e = msg;
    return false;
}
}  // namespace

std::vector<float> AudioBuffer::mono() const {
    const std::size_t n = numFrames();
    std::vector<float> out(n, 0.0f);
    if (channels.empty()) return out;
    const float k = 1.0f / static_cast<float>(channels.size());
    for (const auto& ch : channels)
        for (std::size_t i = 0; i < n && i < ch.size(); ++i) out[i] += ch[i] * k;
    return out;
}

bool readWav(const std::string& path, AudioBuffer& out, std::string* error) {
    drwav wav;
    if (!drwav_init_file(&wav, path.c_str(), nullptr)) return fail(error, "cannot open or parse file");

    const std::size_t nch = wav.channels;
    const std::size_t frames = static_cast<std::size_t>(wav.totalPCMFrameCount);
    if (nch == 0) {
        drwav_uninit(&wav);
        return fail(error, "no channels");
    }

    std::vector<float> interleaved(frames * nch);
    const drwav_uint64 got = drwav_read_pcm_frames_f32(&wav, frames, interleaved.data());
    const int rate = static_cast<int>(wav.sampleRate);
    drwav_uninit(&wav);

    out.sampleRate = rate;
    out.channels.assign(nch, std::vector<float>(static_cast<std::size_t>(got), 0.0f));
    for (std::size_t i = 0; i < got; ++i)
        for (std::size_t c = 0; c < nch; ++c) out.channels[c][i] = interleaved[i * nch + c];
    return true;
}

bool writeWav(const std::string& path, const AudioBuffer& buf, std::string* error) {
    const std::size_t nch = buf.numChannels(), nfr = buf.numFrames();
    if (nch == 0) return fail(error, "no channels");

    drwav_data_format fmt{};
    fmt.container = drwav_container_riff;
    fmt.format = DR_WAVE_FORMAT_IEEE_FLOAT;  // float out: never quantise a B-format render
    fmt.channels = static_cast<drwav_uint32>(nch);
    fmt.sampleRate = static_cast<drwav_uint32>(buf.sampleRate);
    fmt.bitsPerSample = 32;

    drwav wav;
    if (!drwav_init_file_write(&wav, path.c_str(), &fmt, nullptr)) return fail(error, "cannot open file for writing");

    std::vector<float> row(nch);
    for (std::size_t i = 0; i < nfr; ++i) {
        for (std::size_t c = 0; c < nch; ++c) row[c] = i < buf.channels[c].size() ? buf.channels[c][i] : 0.0f;
        drwav_write_pcm_frames(&wav, 1, row.data());
    }
    drwav_uninit(&wav);
    return true;
}

}  // namespace bambi
