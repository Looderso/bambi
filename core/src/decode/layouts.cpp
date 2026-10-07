// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/decode/layouts.hpp"

#include <array>
#include <vector>

namespace bambi {

namespace {
struct Channel {
    const char* name;
    double azimuthDeg, elevationDeg;
    bool lfe;
};
struct System {
    const char* name;
    std::vector<Channel> channels;  // in the order the recommendation lists them: channel i + 1
};

// ITU-R BS.2051-3, table 1: the systems and their channels at the nominal positions, the M layer at 0 degrees, U at
// +30, B at -30, T straight up. Also published as the EBU ADM renderer's 2051_layouts.yaml.
const std::vector<System>& systems() {
    static const std::vector<System> table = {
        {"0+2+0",
         {
             {"M+030", 30, 0, false},
             {"M-030", -30, 0, false},
         }},
        {"0+5+0",
         {
             {"M+030", 30, 0, false},
             {"M-030", -30, 0, false},
             {"M+000", 0, 0, false},
             {"LFE1", 45, -30, true},
             {"M+110", 110, 0, false},
             {"M-110", -110, 0, false},
         }},
        {"2+5+0",
         {
             {"M+030", 30, 0, false},
             {"M-030", -30, 0, false},
             {"M+000", 0, 0, false},
             {"LFE1", 45, -30, true},
             {"M+110", 110, 0, false},
             {"M-110", -110, 0, false},
             {"U+030", 30, 30, false},
             {"U-030", -30, 30, false},
         }},
        {"4+5+0",
         {
             {"M+030", 30, 0, false},
             {"M-030", -30, 0, false},
             {"M+000", 0, 0, false},
             {"LFE1", 45, -30, true},
             {"M+110", 110, 0, false},
             {"M-110", -110, 0, false},
             {"U+030", 30, 30, false},
             {"U-030", -30, 30, false},
             {"U+110", 110, 30, false},
             {"U-110", -110, 30, false},
         }},
        {"4+5+1",
         {
             {"M+030", 30, 0, false},
             {"M-030", -30, 0, false},
             {"M+000", 0, 0, false},
             {"LFE1", 45, -30, true},
             {"M+110", 110, 0, false},
             {"M-110", -110, 0, false},
             {"U+030", 30, 30, false},
             {"U-030", -30, 30, false},
             {"U+110", 110, 30, false},
             {"U-110", -110, 30, false},
             {"B+000", 0, -30, false},
         }},
        {"3+7+0",
         {
             {"M+000", 0, 0, false},
             {"M+030", 30, 0, false},
             {"M-030", -30, 0, false},
             {"U+045", 45, 30, false},
             {"U-045", -45, 30, false},
             {"M+090", 90, 0, false},
             {"M-090", -90, 0, false},
             {"M+135", 135, 0, false},
             {"M-135", -135, 0, false},
             {"UH+180", 180, 45, false},
             {"LFE1", 45, -30, true},
             {"LFE2", -45, -30, true},
         }},
        {"4+9+0",
         {
             {"M+030", 30, 0, false},
             {"M-030", -30, 0, false},
             {"M+000", 0, 0, false},
             {"LFE1", 45, -30, true},
             {"M+090", 90, 0, false},
             {"M-090", -90, 0, false},
             {"M+135", 135, 0, false},
             {"M-135", -135, 0, false},
             {"U+045", 45, 30, false},
             {"U-045", -45, 30, false},
             {"U+135", 135, 30, false},
             {"U-135", -135, 30, false},
             {"M+SC", 15, 0, false},
             {"M-SC", -15, 0, false},
         }},
        {"9+10+3",
         {
             {"M+060", 60, 0, false},  {"M-060", -60, 0, false},  {"M+000", 0, 0, false},
             {"LFE1", 45, -30, true},  {"M+135", 135, 0, false},  {"M-135", -135, 0, false},
             {"M+030", 30, 0, false},  {"M-030", -30, 0, false},  {"M+180", 180, 0, false},
             {"LFE2", -45, -30, true}, {"M+090", 90, 0, false},   {"M-090", -90, 0, false},
             {"U+045", 45, 30, false}, {"U-045", -45, 30, false}, {"U+000", 0, 30, false},
             {"T+000", 0, 90, false},  {"U+135", 135, 30, false}, {"U-135", -135, 30, false},
             {"U+090", 90, 30, false}, {"U-090", -90, 30, false}, {"U+180", 180, 30, false},
             {"B+000", 0, -30, false}, {"B+045", 45, -30, false}, {"B-045", -45, -30, false},
         }},
        {"0+7+0",
         {
             {"M+030", 30, 0, false},
             {"M-030", -30, 0, false},
             {"M+000", 0, 0, false},
             {"LFE1", 45, -30, true},
             {"M+090", 90, 0, false},
             {"M-090", -90, 0, false},
             {"M+135", 135, 0, false},
             {"M-135", -135, 0, false},
         }},
        {"4+7+0",
         {
             {"M+030", 30, 0, false},
             {"M-030", -30, 0, false},
             {"M+000", 0, 0, false},
             {"LFE1", 45, -30, true},
             {"M+090", 90, 0, false},
             {"M-090", -90, 0, false},
             {"M+135", 135, 0, false},
             {"M-135", -135, 0, false},
             {"U+045", 45, 30, false},
             {"U-045", -45, 30, false},
             {"U+135", 135, 30, false},
             {"U-135", -135, 30, false},
         }}};
    return table;
}

const std::vector<std::pair<std::string_view, Layout>>& layouts() {
    static const std::vector<std::pair<std::string_view, Layout>> built = [] {
        std::vector<std::pair<std::string_view, Layout>> out;
        for (const System& sys : systems()) {
            Layout l;
            int channel = 0;
            for (const Channel& c : sys.channels) {
                ++channel;
                if (c.lfe) continue;  // the LFE keeps its channel number as a gap: bass management is not the decoder's
                Loudspeaker s;
                s.azimuthDeg = c.azimuthDeg;
                s.elevationDeg = c.elevationDeg;
                s.channel = channel;
                l.speakers.push_back(s);
            }
            out.emplace_back(sys.name, std::move(l));
        }
        return out;
    }();
    return built;
}
}  // namespace

const Layout* bs2051Layout(std::string_view name) {
    for (const auto& [n, l] : layouts())
        if (n == name) return &l;
    return nullptr;
}

std::vector<std::string_view> bs2051Names() {
    std::vector<std::string_view> names;
    for (const auto& [n, l] : layouts()) names.push_back(n);
    return names;
}

std::string_view bs2051ChannelName(std::string_view layout, int channel) {
    for (const System& sys : systems())
        if (sys.name == layout && channel >= 1 && channel <= static_cast<int>(sys.channels.size()))
            return sys.channels[static_cast<std::size_t>(channel - 1)].name;
    return {};
}

}  // namespace bambi
