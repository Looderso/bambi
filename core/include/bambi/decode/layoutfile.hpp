// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <string_view>

#include "bambi/patch/layout.hpp"

namespace bambi {

/// Reads a layout from the text of an IEM AllRADecoder configuration (the format SPARTA reads and writes too):
/// LoudspeakerLayout.Loudspeakers[] with Azimuth, Elevation, Radius, IsImaginary, Channel, Gain. False, with
/// `error` set, when the text is not one; `out` is then unspecified.
bool parseIemLayout(std::string_view json, Layout& out, std::string* error = nullptr);

/*  Reads a layout from the text of an AmbDec configuration (`.ambdec`: AmbDec itself, the Ambisonic Decoder
 *  Toolbox's output, OpenAL Soft's presets): the `/speakers/{` block's `add_spkr` lines -- an id, the distance in
 *  metres, the azimuth in degrees counter-clockwise from the front, the elevation up, and a JACK port the
 *  loudspeaker connects to. The port's trailing number is the channel (system:playback_7 plays on 7); a port
 *  without one, or none, puts the loudspeaker on the channel of its position in the list. The file's decoder
 *  matrices are not read: the decoder is designed here from the positions. False, with `error` set, when the text
 *  has no speakers block, a line that does not parse, or a `/dec/speakers` count the block does not match. */
bool parseAmbdecLayout(std::string_view text, Layout& out, std::string* error = nullptr);

/// Whichever of the two the text is: an IEM JSON when it opens with `{`, an AmbDec configuration otherwise.
bool parseLayoutFile(std::string_view text, Layout& out, std::string* error = nullptr);

}  // namespace bambi
