// SPDX-License-Identifier: GPL-3.0-or-later
//
//  bambi-live-stub: stands in for the host, to prove the Max for Live device and the host can talk before
//  the host exists. A local TCP server speaking the device's protocol -- one JSON object per line. It
//  answers `hello`, assigns a stereo pair to every top-level track that plays audio when the `tree` comes
//  (with the host's own pair assignment), prints what the device observes, and measures how often Live
//  reports the song position while playing.
//
//    bambi-live-stub [--port 9461] [--pairs 7] [--quit-when-alone SECONDS]
//    bambi-live-stub --names       sets test names on the loop device, reads them back, and exits
//
//  Its state -- the pair assignment -- goes to the device as `save` after every change and comes back as
//  `restore`, so a reopened set finds its tracks on the pairs they had. A second device is answered `busy`.
//  Started by the device, it quits SECONDS after its last device has gone.
//
//  Seven pairs by default: the loop device's channels 1-14, with 15/16 left for the binaural return. On macOS,
//  with bambi's own loop device installed, every assignment also names the device's channels after the tracks.

#include <algorithm>
#include <arpa/inet.h>
#include <chrono>
#include <cstdio>
#include <netinet/in.h>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

#include "bambi/live/pairs.hpp"
#include "bambi/live/protocol.hpp"
#include "bambi/patch/json.hpp"

#ifdef __APPLE__
#include <CoreAudio/CoreAudio.h>
#endif

namespace {

using Clock = std::chrono::steady_clock;
using bambi::Json;
using bambi::live::TrackId;
using bambi::live::TrackPair;

struct Session {
    int fd{-1};
    std::string pending;
    std::vector<TrackPair> pairs;  // the last assignment, so a new tree keeps every track's pair
    int pairCount{7};
    bool playing{false};
    Clock::time_point lastPosition{};
    std::vector<double> intervalsMs;  // between song position reports while playing
};

#ifdef __APPLE__
//  bambi's loop device, by the uid its build gives it; none when it is not installed.
AudioObjectID loopDevice() {
    CFStringRef uid = CFSTR("bambi_UID");
    AudioObjectID id = kAudioObjectUnknown;
    UInt32 size = sizeof id;
    const AudioObjectPropertyAddress a{kAudioHardwarePropertyTranslateUIDToDevice, kAudioObjectPropertyScopeGlobal,
                                       kAudioObjectPropertyElementMain};
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, sizeof uid, &uid, &size, &id);
    return id;
}

/// Names the loop device's channels, channel 1 first, through its custom property 'bnam'. False when the device
/// is not there or refuses.
bool nameChannels(const std::vector<std::string>& names) {
    const AudioObjectID device = loopDevice();
    if (device == kAudioObjectUnknown) return false;
    CFMutableArrayRef list = CFArrayCreateMutable(nullptr, static_cast<CFIndex>(names.size()), &kCFTypeArrayCallBacks);
    for (const std::string& n : names) {
        CFStringRef str = CFStringCreateWithCString(nullptr, n.c_str(), kCFStringEncodingUTF8);
        CFArrayAppendValue(list, str);
        CFRelease(str);
    }
    const AudioObjectPropertyAddress a{'bnam', kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    CFPropertyListRef value = list;
    const OSStatus status = AudioObjectSetPropertyData(device, &a, 0, nullptr, sizeof value, &value);
    CFRelease(list);
    return status == noErr;
}

/// A channel's name as any program reads it: the device's element name.
std::string channelName(int channel) {
    const AudioObjectID device = loopDevice();
    CFStringRef name = nullptr;
    UInt32 size = sizeof name;
    const AudioObjectPropertyAddress a{kAudioObjectPropertyElementName, kAudioObjectPropertyScopeOutput,
                                       static_cast<AudioObjectPropertyElement>(channel)};
    if (device == kAudioObjectUnknown || AudioObjectGetPropertyData(device, &a, 0, nullptr, &size, &name) != noErr ||
        name == nullptr)
        return {};
    char buf[256] = {};
    CFStringGetCString(name, buf, sizeof buf, kCFStringEncodingUTF8);
    CFRelease(name);
    return buf;
}
#endif

namespace live = bambi::live;

void send(Session& s, const live::Message& message) {
    const std::string line = live::serialise(message) + "\n";
    if (s.fd >= 0) (void)::write(s.fd, line.data(), line.size());
}

const char* kindName(live::Node::Kind k) {
    return k == live::Node::Kind::Group ? "group" : k == live::Node::Kind::Return ? "return" : "track";
}

void onTree(Session& s, const live::Tree& tree) {
    std::vector<TrackId> top;
    std::vector<TrackId> sending;  // tracks with a send up: the test warning's subject
    std::printf("tree: %zu nodes\n", tree.nodes.size());
    for (const live::Node& n : tree.nodes) {
        const bool elsewhere = n.path == live::Node::Path::Elsewhere;
        std::printf("  %-6s %-24s id %-6llu %s%s\n", kindName(n.kind), n.name.c_str(),
                    static_cast<unsigned long long>(n.id),
                    elsewhere   ? "not on a path to Main"
                    : !n.parent ? "top"
                                : "into its group",
                    n.audio ? "" : ", no audio");
        if (!n.parent && !elsewhere && n.kind != live::Node::Kind::Return && n.audio) top.push_back(n.id);
        for (const auto& send : n.sends)
            if (send) {
                sending.push_back(n.id);
                break;
            }
    }
    const live::PairAssignment a = live::assignPairs(s.pairs, top, s.pairCount);
    s.pairs = a.pairs;
    send(s, live::Route{a.pairs});
    std::printf("route: %zu tracks on pairs, %zu left over\n", a.pairs.size(), a.unassigned.size());

    //  the host's state, kept by the device so the set saves it
    Json state = Json::object();
    Json pairs = Json::array();
    for (const TrackPair& p : a.pairs) {
        Json t = Json::object();
        t.set("id", Json(static_cast<double>(p.track)));
        t.set("pair", Json(p.pair));
        pairs.push(std::move(t));
    }
    state.set("pairs", std::move(pairs));
    send(s, live::Save{std::move(state)});

    //  what the host would say about the mix; the stub's are test values, a warning on every routed track
    //  with a send up, so the device's list can be seen to mark it
    live::Info info{12.0, 48000.0, {}};
    for (const TrackPair& p : a.pairs)
        if (std::find(sending.begin(), sending.end(), p.track) != sending.end())
            info.warnings[p.track] = "its sends go to Live's returns, which stay stereo here";
    send(s, info);
#ifdef __APPLE__
    std::vector<std::string> channels(static_cast<std::size_t>(2 * s.pairCount + 2));
    for (const TrackPair& p : a.pairs)
        for (const live::Node& n : tree.nodes)
            if (n.id == p.track) {
                channels[static_cast<std::size_t>(2 * p.pair)] = n.name + " L";
                channels[static_cast<std::size_t>(2 * p.pair + 1)] = n.name + " R";
            }
    channels[static_cast<std::size_t>(2 * s.pairCount)] = "bambi return L";
    channels[static_cast<std::size_t>(2 * s.pairCount + 1)] = "bambi return R";
    std::printf("loop device channels %s\n", nameChannels(channels) ? "named" : "not named (no bambi loop device)");
#endif
}

void onTransport(Session& s, const live::TransportMessage& m) {
    const auto now = Clock::now();
    if (m.playing && s.playing)
        s.intervalsMs.push_back(std::chrono::duration<double, std::milli>(now - s.lastPosition).count());
    if (m.playing != s.playing)
        std::printf("transport: %s at %.3f, %.2f bpm\n", m.playing ? "playing" : "stopped", m.ppq, m.bpm);
    s.playing = m.playing;
    s.lastPosition = now;
}

void report(Session& s) {
    if (s.intervalsMs.size() < 2) return;
    std::vector<double> v = s.intervalsMs;
    std::sort(v.begin(), v.end());
    double sum = 0.0;
    for (const double x : v) sum += x;
    std::printf("song position: %zu reports, every %.1f ms on average, 95%% within %.1f ms, longest %.1f ms\n",
                v.size(), sum / static_cast<double>(v.size()), v[v.size() * 95 / 100], v.back());
    s.intervalsMs.clear();
}

//  What the device says, answered as the host will answer it.
struct Handler {
    Session& s;
    void operator()(const live::Hello& h) const {
        std::printf("hello: protocol %d, Live %s, %.0f Hz%s\n", h.protocol, h.live.c_str(), h.rate,
                    live::accepts(h) ? "" : " -- not a protocol this host speaks");
        send(s, live::Welcome{live::kProtocol, true});
    }
    void operator()(const live::Restore& r) const {
        s.pairs.clear();
        if (const Json* pairs = r.state.find("pairs"))
            for (const Json& p : pairs->items()) {
                const Json* id = p.find("id");
                const Json* pair = p.find("pair");
                if (id != nullptr && pair != nullptr)
                    s.pairs.push_back({static_cast<TrackId>(id->numberOr(0)), static_cast<int>(pair->numberOr(0))});
            }
        std::printf("restore: %zu tracks' pairs from the set\n", s.pairs.size());
    }
    void operator()(const live::Tree& t) const { onTree(s, t); }
    void operator()(const live::TransportMessage& m) const { onTransport(s, m); }
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

void handle(Session& s, const std::string& line) {
    std::string error;
    const auto m = live::parse(line, &error);
    if (!m) {
        std::printf("not understood (%s): %s\n", error.c_str(), line.c_str());
        return;
    }
    std::visit(Handler{s}, *m);
}

}  // namespace

int main(int argc, char** argv) {
    int port = 9461;
    int quitAfter = -1;  // seconds alone before quitting; never by default
    Session s;
#ifdef __APPLE__
    if (argc > 1 && std::string(argv[1]) == "--names") {
        std::vector<std::string> names;
        for (int c = 1; c <= 16; ++c) names.push_back("test " + std::to_string(c));
        if (!nameChannels(names)) {
            std::printf("no bambi loop device, or it refused the names\n");
            return 1;
        }
        for (int c = 1; c <= 16; ++c) std::printf("channel %2d: %s\n", c, channelName(c).c_str());
        return 0;
    }
#endif
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string key = argv[i];
        if (key == "--port")
            port = std::stoi(argv[i + 1]);
        else if (key == "--pairs")
            s.pairCount = std::stoi(argv[i + 1]);
        else if (key == "--quit-when-alone")
            quitAfter = std::stoi(argv[i + 1]);
    }
    const int server = ::socket(AF_INET, SOCK_STREAM, 0);
    const int on = 1;
    ::setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<std::uint16_t>(port));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(server, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 || ::listen(server, 1) != 0) {
        std::fprintf(stderr, "bambi-live-stub: cannot listen on 127.0.0.1:%d\n", port);
        return 1;
    }
    std::printf("listening on 127.0.0.1:%d, %d pairs; ctrl-c stops\n", port, s.pairCount);
    std::setvbuf(stdout, nullptr, _IOLBF, 0);

    auto lastReport = Clock::now();
    auto alone = Clock::now();  // since the last device went, or since the start
    for (;;) {
        pollfd fds[2] = {{server, POLLIN, 0}, {s.fd, POLLIN, 0}};
        ::poll(fds, s.fd >= 0 ? 2 : 1, 1000);
        if (fds[0].revents & POLLIN) {
            const int client = ::accept(server, nullptr, nullptr);
            if (s.fd >= 0) {  // one device at a time: the first is answered, a second is told so
                const std::string busy = bambi::live::serialise(bambi::live::Busy{}) + "\n";
                (void)::write(client, busy.data(), busy.size());
                ::close(client);
            } else {
                s.fd = client;
                s.pending.clear();
                std::printf("device connected\n");
            }
        }
        if (s.fd >= 0 && (fds[1].revents & (POLLIN | POLLHUP))) {
            char buf[4096];
            const ssize_t n = ::read(s.fd, buf, sizeof buf);
            if (n <= 0) {
                ::close(s.fd);
                s.fd = -1;
                std::printf("device gone\n");
                alone = Clock::now();
            } else {
                s.pending.append(buf, static_cast<std::size_t>(n));
                for (std::size_t at; (at = s.pending.find('\n')) != std::string::npos;) {
                    const std::string line = s.pending.substr(0, at);
                    s.pending.erase(0, at + 1);
                    if (!line.empty()) handle(s, line);
                }
            }
        }
        if (s.fd < 0 && quitAfter >= 0 && Clock::now() - alone > std::chrono::seconds(quitAfter)) {
            std::printf("alone for %d s: quitting\n", quitAfter);
            return 0;
        }
        if (Clock::now() - lastReport > std::chrono::seconds(5)) {
            report(s);
            lastReport = Clock::now();
        }
    }
}
