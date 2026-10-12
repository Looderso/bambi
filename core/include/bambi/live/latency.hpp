// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <vector>

/*  How much the host delays each of its paths so that they meet in step. The host has one shape: every
 *  source is an encoder and its inserts; each source feeds the dry sum and every send bus, post-insert;
 *  each send bus runs its own chain; the dry sum and the send returns meet, then run the main chain and
 *  the decoder. Two places paths meet, so two places they are lined up:
 *
 *    - the sources' outputs, which feed the dry sum and every send at once: each source is delayed to the
 *      slowest source, so a send hears its sources together and the dry sum too;
 *    - the main sum: the dry signal is delayed to the slowest send chain, each return by what it lacks.
 *
 *  A path's latency is in samples, its own plugins' reports added up; a negative one counts as 0. Each
 *  delay is the least that lines the paths up -- the slowest path at each meeting place gets none.
 */
namespace bambi::live {

struct HostLatency {
    std::vector<int> sources;  ///< per source: its encoder and inserts
    std::vector<int> sends;    ///< per send bus: its chain
    int main{0};               ///< the main chain and the decoder
};

struct Compensation {
    std::vector<int> sourceDelay;  ///< per source, at its output
    int dryDelay{0};               ///< the dry sum, before it meets the returns
    std::vector<int> returnDelay;  ///< per send bus, at its return
    int total{0};                  ///< from a source's input to the output, the same on every path
};

Compensation compensate(const HostLatency& latency);

}  // namespace bambi::live
