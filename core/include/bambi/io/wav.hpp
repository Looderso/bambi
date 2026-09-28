// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <string>
#include <vector>

namespace bambi {

/*  Minimal WAV, enough for offline rendering and golden-file fixtures. Lives in core so a
 *  fixture can be rendered and hashed headlessly, without a DAW; the plugin itself never
 *  uses this — the host reads files.
 *
 *  Reads 16/24/32-bit PCM and 32/64-bit float. Writes 32-bit float, which is what every
 *  ambisonic tool expects and what avoids quantising a B-format render twice.
 */
struct AudioBuffer {
    int sampleRate{48000};
    std::vector<std::vector<float>> channels;  ///< planar

    std::size_t numChannels() const { return channels.size(); }
    std::size_t numFrames() const { return channels.empty() ? 0 : channels[0].size(); }

    /// Sum to one channel. An encoder takes a single signal.
    std::vector<float> mono() const;
};

/// Returns false and fills `error` on failure; never throws.
bool readWav(const std::string& path, AudioBuffer& out, std::string* error = nullptr);
bool writeWav(const std::string& path, const AudioBuffer& buf, std::string* error = nullptr);

}  // namespace bambi
