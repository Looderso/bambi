// SPDX-License-Identifier: GPL-3.0-or-later
//
//  bambi-link — exercise the link bus from a shell, in separate processes.
//
//  The bus exists to carry state between processes, and a test that runs two LinkBus objects
//  inside one process does not test that: it shares an address space, a compiler and a
//  clock with itself. Hosts sandbox plugins across processes, which is the case that has to
//  work, so it gets a tool rather than an assumption.
//
//  `publish` is shaped like a plugin on purpose. One thread plays the audio callback and
//  calls nothing but LinkPublisher::submit; the main thread plays the message-thread timer.
//  So the cross-process test exercises the real hand-off rather than a shortcut around it.

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "bambi/encode/params.hpp"
#include "bambi/link/link.hpp"
#include "bambi/math/vec3.hpp"
#include "bambi/path/generator.hpp"
#include "bambi/path/trajectory.hpp"

using namespace bambi;
using Clock = std::chrono::steady_clock;

namespace {

void usage() {
    std::printf(R"(bambi-link — link bus exerciser

  bambi-link publish --session UUID [--label NAME] [--seconds S]
                      [--reshape-after S] [--audio-stops-after S]
  bambi-link watch   --session UUID [--seconds S] [--expect N] [--expect-reshape]
  bambi-link send    --session UUID --target UUID --param KEY --value V
                      [--gesture begin|end] [--hold S]
  bambi-link unlink  --session UUID

publish  joins the session the way a plugin does: an "audio" thread moves a source along
         an orbit and submits where it is; the main thread publishes at 60 Hz
         and turns remote edits into the host actions a plugin would perform.
         --reshape-after switches the path to a lissajous: a static-section edit.
         --audio-stops-after ends the audio thread early while the timer carries on,
         the way a host that stops calling the audio callback would.
watch    prints the scene. With --expect it exits nonzero unless exactly N other
         instances were seen on every frame; with --expect-reshape, unless some
         instance's path changed while it was watching.
send     queues one parameter edit on another instance, the way editing a selected
         instance from a different window does. --gesture marks it as a gesture
         endpoint; --hold keeps the sender alive S seconds afterwards, so a script can
         kill it in the middle of an edit
unlink   removes the session's shared segment
)");
}

const char* argAfter(int argc, char** argv, const char* flag) {
    for (int i = 1; i + 1 < argc; ++i)
        if (std::strcmp(argv[i], flag) == 0) return argv[i + 1];
    return nullptr;
}

bool hasFlag(int argc, char** argv, const char* flag) {
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], flag) == 0) return true;
    return false;
}

double secondsSince(Clock::time_point t0) { return std::chrono::duration<double>(Clock::now() - t0).count(); }

Trajectory buildPath(GeneratorType g) {
    TrajectoryState ts;
    ts.kind = TrajectoryKind::Parametric;
    ts.generator = g;
    generatorDefaults(g, ts.genParams);
    Trajectory t;
    t.build(ts);
    return t;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        usage();
        return 1;
    }
    const std::string cmd = argv[1];

    const char* sessionArg = argAfter(argc, argv, "--session");
    if (sessionArg == nullptr) {
        std::fprintf(stderr, "--session is required\n");
        return 2;
    }
    const auto session = Uuid::parse(sessionArg);
    if (!session) {
        std::fprintf(stderr, "--session is not a uuid\n");
        return 2;
    }

    const char* secArg = argAfter(argc, argv, "--seconds");
    const double seconds = secArg ? std::atof(secArg) : 2.0;

    if (cmd == "unlink") {
        LinkBus::unlinkSession(*session);
        std::printf("unlinked\n");
        return 0;
    }

    const Uuid self = Uuid::generate();
    LinkBus bus;
    if (!bus.open(*session, self, Product::Encoder)) {
        std::fprintf(stderr, "open failed: %s\n", bus.error().c_str());
        return 1;
    }

    if (cmd == "publish") {
        const char* label = argAfter(argc, argv, "--label");
        const char* reshapeArg = argAfter(argc, argv, "--reshape-after");
        const char* stopsArg = argAfter(argc, argv, "--audio-stops-after");
        const double reshapeAfter = reshapeArg ? std::atof(reshapeArg) : -1.0;
        const double audioStopsAfter = stopsArg ? std::atof(stopsArg) : -1.0;

        std::printf("%s\n", self.toString().c_str());  // so a script can address us
        std::fflush(stdout);

        //  Both paths are built before the audio thread starts and never modified, so it can
        //  switch between them through one atomic. Handing a rebuilt trajectory to a real
        //  audio thread is the plugin's own problem; this tool only has to prove that the bus
        //  carries the edit.
        const Trajectory paths[2] = {buildPath(GeneratorType::Orbit), buildPath(GeneratorType::Lissajous)};
        std::atomic<int> shape{0};

        LinkStatic st;
        if (label != nullptr) st.setLabel(label);
        st.colour = 2;
        st.order = 3;
        st.setPath(paths[0]);
        bus.publishStatic(st);

        LinkPublisher publisher(bus);
        std::atomic<bool> stop{false}, audioStopped{false};
        const auto t0 = Clock::now();

        std::thread audio([&] {
            constexpr double kHop = 256.0 / 48000.0;
            double s = 0.0, t = 0.0;
            while (!stop.load(std::memory_order_relaxed)) {
                if (audioStopsAfter >= 0.0 && secondsSince(t0) > audioStopsAfter) break;
                const Trajectory& path = paths[shape.load(std::memory_order_relaxed)];
                s += (60.0 * kDeg2Rad / path.lengthRad()) * kHop;  // 60 deg/s along the path
                s -= std::floor(s);
                t += kHop;
                const Vec3 p = path.eval(s);

                LinkDynamic d;
                d.s = static_cast<float>(s);
                d.x = static_cast<float>(p.x);
                d.y = static_cast<float>(p.y);
                d.z = static_cast<float>(p.z);
                d.liveCount = static_cast<std::uint32_t>(EncoderParam::RenderWidth) + 1;
                d.live[static_cast<std::size_t>(EncoderParam::RenderWidth)] = 30.0f;
                d.level = static_cast<float>(0.5 + 0.5 * std::sin(t * 6.0));
                publisher.submit(d);  // the only bus-related call on this thread
                std::this_thread::sleep_for(std::chrono::duration<double>(kHop));
            }
            audioStopped.store(true);
        });

        int commandsSeen = 0, rejoinsReported = 0;
        bool reshaped = false, reportedStop = false;
        std::vector<LinkCommand> cmds;
        LinkEditReceiver receiver;
        std::vector<LinkEditAction> edits;
        std::vector<LinkPeer> live;
        while (secondsSince(t0) < seconds) {
            publisher.tick();

            if (bus.rejoins() != rejoinsReported) {
                rejoinsReported = bus.rejoins();
                std::printf("rejoined the session in slot %d\n", bus.selfSlot());
                std::fflush(stdout);
            }
            if (!reshaped && reshapeAfter >= 0.0 && secondsSince(t0) > reshapeAfter) {
                st.setPath(paths[1]);
                bus.publishStatic(st);
                shape.store(1);
                reshaped = true;
                std::printf("reshaped\n");
                std::fflush(stdout);
            }
            if (!reportedStop && audioStopped.load()) {
                reportedStop = true;
                std::printf("audio stopped\n");
                std::fflush(stdout);
            }

            bus.drainCommands(cmds);
            for (const auto& c : cmds) {
                ++commandsSeen;
                std::printf("command %.*s = %.3f from %s\n", static_cast<int>(parameter(c.paramId()).key.size()),
                            parameter(c.paramId()).key.data(), static_cast<double>(c.value), c.from.toString().c_str());
                std::fflush(stdout);
            }

            //  What the plugin would hand its host. Polling the live peers every tick is what
            //  lets the gesture of a sender that has died be closed instead of left open.
            edits.clear();
            receiver.apply(cmds, edits);
            bus.poll(live);
            receiver.closeAbandoned(live, edits);
            for (const auto& a : edits) {
                const auto key = parameter(a.param).key;
                if (a.kind == LinkEditAction::Kind::Value) {
                    std::printf("edit value %.*s = %.3f\n", static_cast<int>(key.size()), key.data(),
                                static_cast<double>(a.value));
                } else {
                    std::printf("edit %s %.*s%s\n", a.kind == LinkEditAction::Kind::Begin ? "begin" : "end",
                                static_cast<int>(key.size()), key.data(), a.abandoned ? " (sender gone)" : "");
                }
                std::fflush(stdout);
            }
            std::this_thread::sleep_for(std::chrono::duration<double>(1.0 / 60.0));
        }
        stop.store(true);
        audio.join();
        std::printf("published, %d commands received\n", commandsSeen);
        return 0;
    }

    if (cmd == "watch") {
        const char* expectArg = argAfter(argc, argv, "--expect");
        const int expect = expectArg ? std::atoi(expectArg) : -1;
        const bool expectReshape = hasFlag(argc, argv, "--expect-reshape");

        const auto t0 = Clock::now();
        LinkScene scene;
        int frames = 0, wrong = 0, maxSeen = 0, firstWrong = -1;
        std::vector<int> histogram(kLinkMaxInstances + 1, 0);
        struct Generations {
            Uuid instance{};
            std::uint32_t first{0}, last{0};
        };
        std::vector<Generations> generations;

        while (secondsSince(t0) < seconds) {
            scene.update(bus);
            //  The watcher occupies a slot too, so it leaves itself out: what a scene window
            //  wants to know is how many other instances there are.
            int others = 0;
            for (const auto& e : scene.entries()) {
                if (e.instance == self) continue;
                ++others;
                if (e.generation == 0) continue;
                Generations* g = nullptr;
                for (auto& x : generations)
                    if (x.instance == e.instance) g = &x;
                if (g == nullptr)
                    generations.push_back({e.instance, e.generation, e.generation});
                else
                    g->last = e.generation;
            }
            maxSeen = std::max(maxSeen, others);
            ++histogram[static_cast<std::size_t>(others)];
            if (expect >= 0 && others != expect) {
                if (firstWrong < 0) firstWrong = frames;
                ++wrong;
            }
            ++frames;
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }

        for (const auto& e : scene.entries()) {
            if (e.instance == self) continue;
            if (!e.hasPosition) {
                std::printf("  %-12s no position yet  path gen %u, points %u\n", e.st.labelString().c_str(),
                            e.generation, e.st.pointCount);
                continue;
            }
            const Vec3 p = e.dyn.position();
            std::printf(
                "  %-12s az %7.1f  el %6.1f  s %.3f  width %5.1f  level %.2f  "
                "path gen %u, points %u, %s\n",
                e.st.labelString().c_str(), azimuth(p) * kRad2Deg, elevation(p) * kRad2Deg,
                static_cast<double>(e.dyn.s),
                static_cast<double>(e.dyn.live[static_cast<std::size_t>(EncoderParam::RenderWidth)]),
                static_cast<double>(e.dyn.level), e.generation, e.st.pointCount, e.st.closed ? "closed" : "open");
        }
        std::printf("watched %d frames, peak %d peers, %llu path copies\n", frames, maxSeen,
                    static_cast<unsigned long long>(scene.staticReads()));

        int failures = 0;
        if (expect >= 0 && wrong > 0) {
            std::fprintf(stderr,
                         "expected %d peers; %d of %d frames disagreed, first at "
                         "frame %d. counts seen:",
                         expect, wrong, frames, firstWrong);
            for (std::size_t i = 0; i < histogram.size(); ++i)
                if (histogram[i] > 0) std::fprintf(stderr, " %zux%d", i, histogram[i]);
            std::fprintf(stderr, "\n");
            ++failures;
        }
        if (expectReshape) {
            bool any = false;
            for (const auto& g : generations)
                if (g.last > g.first) any = true;
            if (!any) {
                std::fprintf(stderr, "expected a path to change while watching; none did\n");
                ++failures;
            }
        }
        return failures > 0 ? 1 : 0;
    }

    if (cmd == "send") {
        const char* targetArg = argAfter(argc, argv, "--target");
        const char* paramArg = argAfter(argc, argv, "--param");
        const char* valueArg = argAfter(argc, argv, "--value");
        if (!targetArg || !paramArg || !valueArg) {
            std::fprintf(stderr, "send needs --target, --param and --value\n");
            return 2;
        }
        const auto target = Uuid::parse(targetArg);
        if (!target) {
            std::fprintf(stderr, "--target is not a uuid\n");
            return 2;
        }
        const ParamId id = parameterByKey(paramArg);
        if (id == kNoParamId) {
            std::fprintf(stderr, "no parameter '%s'\n", paramArg);
            return 2;
        }

        LinkCommand c;
        c.param = static_cast<std::uint32_t>(id);
        c.value = static_cast<float>(std::atof(valueArg));
        if (const char* g = argAfter(argc, argv, "--gesture")) {
            if (std::strcmp(g, "begin") == 0)
                c.gesture = kLinkGestureBegin;
            else if (std::strcmp(g, "end") == 0)
                c.gesture = kLinkGestureEnd;
            else {
                std::fprintf(stderr, "--gesture is begin or end\n");
                return 2;
            }
        }
        if (!bus.sendCommand(*target, c)) {
            std::fprintf(stderr, "target not reachable\n");
            return 1;
        }
        std::printf("sent\n");
        std::fflush(stdout);

        //  Stay alive, heartbeat and all, the way a window in the middle of a drag would, so a
        //  script can kill -9 this process to play a sender that dies mid-edit.
        const char* holdArg = argAfter(argc, argv, "--hold");
        const double hold = holdArg ? std::atof(holdArg) : 0.0;
        const auto t0 = Clock::now();
        while (secondsSince(t0) < hold) {
            bus.publishDynamic(LinkDynamic{});
            std::this_thread::sleep_for(std::chrono::duration<double>(1.0 / 60.0));
        }
        return 0;
    }

    usage();
    return 2;
}
