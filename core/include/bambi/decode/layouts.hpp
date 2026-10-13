// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string_view>
#include <vector>

#include "bambi/patch/layout.hpp"

namespace bambi {

/*  The ITU-R BS.2051 layouts, built in, by the recommendation's names: "0+2+0" (stereo), "0+5+0", "2+5+0",
 *  "4+5+0", "4+5+1", "3+7+0", "4+9+0", "9+10+3", "0+7+0" and "4+7+0" (7.1.4). The channels come in the order
 *  the recommendation lists them, so channel numbers follow the common interleaving (L R C LFE ...), and the LFE
 *  channel is left as a gap: the decoder plays nothing there. All loudspeakers at one metre. */
const Layout* bs2051Layout(std::string_view name);  ///< null for a name that is not one of them
std::vector<std::string_view> bs2051Names();
/// The recommendation's label of a channel, "M+030" and the like, or empty.

}  // namespace bambi
