// SPDX-License-Identifier: GPL-3.0-or-later
//
//  bambi-live-stub: stands in for the host, to prove the Max for Live device and the host can talk without the
//  host. A local TCP server around bambi-live's DeviceSession -- the host's own answers -- which prints what the
//  device says and measures how often Live reports the song position while playing.
//
//    bambi-live-stub [--port 9461] [--pairs 7] [--quit-when-alone SECONDS]
//    bambi-live-stub --names       sets test names on the loop device, reads them back, and exits
//
//  Seven pairs by default: the loop device's channels 1-14, with 15/16 left for the binaural return. With bambi's
//  own loop device installed, every assignment names its channels after the tracks. A second device is answered
//  busy. Started by the device, it quits SECONDS after its last device has gone.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <variant>
#include <vector>

#include "bambi/live/protocol.hpp"
#include "bambi/live/session.hpp"
#include "lines.hpp"
#include "loop.hpp"

namespace {

using Clock = std::chrono::steady_clock;
namespace live = bambi::live;

struct Stub {
    bambi::net::LineServer server{live::serialise(live::Busy{})};
    bambi::net::LineServer::Connection device{0};
    live::DeviceSession session;
    bool playing{false};
    Clock::time_point lastPosition{};
    std::vector<double> intervalsMs;  // between song position reports while playing
};

const char* kindName(live::Node::Kind k) {
    return k == live::Node::Kind::Group ? "group" : k == live::Node::Kind::Return ? "return" : "track";
}

//  What the device said, as the stand-in shows it.
struct Print {
    Stub& s;
    void operator()(const live::Hello& h) const {
        std::printf("hello: protocol %d, Live %s%s\n", h.protocol, h.live.c_str(),
                    live::accepts(h) ? "" : " -- not a protocol this host speaks");
    }
    void operator()(const live::Restore&) const {
        std::printf("restore: %zu tracks' pairs from the set\n", s.session.pairs().size());
    }
    void operator()(const live::Tree& t) const {
        std::printf("tree: %zu nodes\n", t.nodes.size());
        for (const live::Node& n : t.nodes)
            std::printf("  %-6s %-24s id %-6llu %s%s\n", kindName(n.kind), n.name.c_str(),
                        static_cast<unsigned long long>(n.id),
                        n.path == live::Node::Path::Elsewhere ? "not on a path to Main"
                        : !n.parent                           ? "top"
                                                              : "into its group",
                        n.audio ? "" : ", no audio");
        std::printf("route: %zu tracks on pairs, %zu left over\n", s.session.pairs().size(),
                    s.session.unassigned().size());
        std::printf("loop device channels %s\n",
                    bambi::loop::nameChannels(s.session.channelNames()) ? "named" : "not named (no bambi loop device)");
    }
    void operator()(const live::TransportMessage& m) const {
        const auto now = Clock::now();
        if (m.playing && s.playing)
            s.intervalsMs.push_back(std::chrono::duration<double, std::milli>(now - s.lastPosition).count());
        if (m.playing != s.playing)
            std::printf("transport: %s at %.3f, %.2f bpm\n", m.playing ? "playing" : "stopped", m.ppq, m.bpm);
        s.playing = m.playing;
        s.lastPosition = now;
    }
    void operator()(const live::Routed& r) const {
        for (const auto& t : r.tracks)
            std::printf("routed: %-24s %s\n", t.name.c_str(), t.ok ? t.to.c_str() : t.error.c_str());
    }
    void operator()(const live::Value& v) const {
        std::printf("value: %-24s %-12s %s\n", v.name.c_str(), v.what.c_str(), v.value.dump(0).c_str());
    }
    void operator()(const live::Show&) const { std::printf("show: the host's window would come to the front\n"); }
    void operator()(const live::Switched& b) const { std::printf("bambi %s\n", b.on ? "on" : "off"); }
    template <typename Other>
    void operator()(const Other&) const {
        std::printf("a message only the host sends, from the device\n");
    }
};

void handle(Stub& s, const std::string& line) {
    std::string error;
    const auto m = live::parse(line, &error);
    if (!m) {
        std::printf("not understood (%s): %s\n", error.c_str(), line.c_str());
        return;
    }
    for (const live::Message& answer : s.session.answer(*m)) s.server.send(s.device, live::serialise(answer));
    std::visit(Print{s}, *m);
}

void report(Stub& s) {
    if (s.intervalsMs.size() < 2) return;
    std::vector<double> v = s.intervalsMs;
    std::sort(v.begin(), v.end());
    double sum = 0.0;
    for (const double x : v) sum += x;
    std::printf("song position: %zu reports, every %.1f ms on average, 95%% within %.1f ms, longest %.1f ms\n",
                v.size(), sum / static_cast<double>(v.size()), v[v.size() * 95 / 100], v.back());
    s.intervalsMs.clear();
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "--names") {
        std::vector<std::string> names;
        for (int c = 1; c <= 16; ++c) names.push_back("test " + std::to_string(c));
        if (!bambi::loop::nameChannels(names)) {
            std::printf("no bambi loop device, or it refused the names\n");
            return 1;
        }
        for (int c = 1; c <= 16; ++c) std::printf("channel %2d: %s\n", c, bambi::loop::channelName(c).c_str());
        return 0;
    }
    int port = 9461, quitAfter = -1, pairs = 7;  // quitAfter: seconds alone before quitting; never by default
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string key = argv[i];
        if (key == "--port")
            port = std::stoi(argv[i + 1]);
        else if (key == "--pairs")
            pairs = std::stoi(argv[i + 1]);
        else if (key == "--quit-when-alone")
            quitAfter = std::stoi(argv[i + 1]);
    }
    Stub s;
    s.session = live::DeviceSession({pairs, bambi::loop::installed(), 12.0, 48000.0});
    if (std::string error; !s.server.listen(port, &error)) {
        std::fprintf(stderr, "bambi-live-stub: cannot listen on 127.0.0.1:%d: %s\n", port, error.c_str());
        return 1;
    }
    std::printf("listening on 127.0.0.1:%d, %d pairs; ctrl-c stops\n", port, pairs);
    std::setvbuf(stdout, nullptr, _IOLBF, 0);

    auto lastReport = Clock::now();
    auto alone = Clock::now();  // since the last device went, or since the start
    using Connection = bambi::net::LineServer::Connection;
    const bambi::net::LineServer::Events events{[&](Connection c) {
                                                    s.device = c;
                                                    std::printf("device connected\n");
                                                },
                                                [&](Connection, const std::string& line) { handle(s, line); },
                                                [&](Connection) {
                                                    s.session.disconnected();
                                                    std::printf("device gone\n");
                                                    alone = Clock::now();
                                                }};
    for (;;) {
        s.server.poll(1000, events);
        if (!s.server.connected() && quitAfter >= 0 && Clock::now() - alone > std::chrono::seconds(quitAfter)) {
            std::printf("alone for %d s: quitting\n", quitAfter);
            return 0;
        }
        if (Clock::now() - lastReport > std::chrono::seconds(5)) {
            report(s);
            lastReport = Clock::now();
        }
    }
}
