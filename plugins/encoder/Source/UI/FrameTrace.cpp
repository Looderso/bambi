// SPDX-License-Identifier: GPL-3.0-or-later
#include "UI/FrameTrace.h"

#include <cstdio>

namespace bambi::ui {
namespace {
constexpr std::size_t kReserveBytes = std::size_t{4} << 20;

//  One schema for every row, so the file loads as one table: fields a kind does not have stay empty.
constexpr const char* kHeader =
    "kind,frame,us,instance,self,has_position,s,x,y,z,sampled_us,published_us,samples,timeline\n";
}  // namespace

FrameTrace::~FrameTrace() {
    if (recording_) processor_.setTracing(false);
}

std::int64_t FrameTrace::since(std::uint64_t us) const {
    return static_cast<std::int64_t>(us) - static_cast<std::int64_t>(startUs_);
}

void FrameTrace::start(double seconds) {
    std::vector<BambiEncoderProcessor::TraceEvent> stale;
    processor_.drainTrace(stale);  // whatever was still queued from a trace before
    rows_.clear();
    rows_.reserve(kReserveBytes);
    rows_ += kHeader;
    events_.clear();
    frames_ = 0;
    startUs_ = bambi::linkNowMicros();
    endUs_ = startUs_ + static_cast<std::uint64_t>(seconds * 1.0e6);
    processor_.setTracing(true);
    recording_ = true;
    status_ = "recording " + juce::String(juce::roundToInt(seconds)) + " s -- play the project";
}

void FrameTrace::frame(const bambi::LinkScene& scene) {
    if (!recording_) return;
    const auto now = bambi::linkNowMicros();
    const auto self = processor_.identity().instance;
    char line[320];

    std::snprintf(line, sizeof line, "frame,%lld,%lld,,,,,,,,,,,\n", static_cast<long long>(frames_),
                  static_cast<long long>(since(now)));
    rows_ += line;
    for (const auto& e : scene.entries()) {
        std::snprintf(line, sizeof line, "peer,%lld,%lld,%.8s,%d,%d,%.6f,%.6f,%.6f,%.6f,%lld,%lld,,\n",
                      static_cast<long long>(frames_), static_cast<long long>(since(now)),
                      e.instance.toString().c_str(), e.instance == self ? 1 : 0, e.hasPosition ? 1 : 0,
                      static_cast<double>(e.dyn.s), static_cast<double>(e.dyn.x), static_cast<double>(e.dyn.y),
                      static_cast<double>(e.dyn.z), static_cast<long long>(since(e.dyn.sampledUs)),
                      static_cast<long long>(since(e.dyn.publishedUs)));
        rows_ += line;
    }

    processor_.drainTrace(events_);
    for (const auto& ev : events_) {
        if (ev.kind == BambiEncoderProcessor::kTraceBlock)
            std::snprintf(line, sizeof line, "block,,%lld,,,,,,,,,,%lld,%lld\n", static_cast<long long>(since(ev.us)),
                          static_cast<long long>(ev.samples), static_cast<long long>(ev.timeline));
        else
            std::snprintf(line, sizeof line, "tick,,%lld,,,,,,,,,,,\n", static_cast<long long>(since(ev.us)));
        rows_ += line;
    }
    events_.clear();

    ++frames_;
    if (now >= endUs_) finish();
}

void FrameTrace::painted() {
    if (!recording_) return;
    char line[96];
    std::snprintf(line, sizeof line, "paint,%lld,%lld,,,,,,,,,,,\n", static_cast<long long>(frames_),
                  static_cast<long long>(since(bambi::linkNowMicros())));
    rows_ += line;
}

void FrameTrace::finish() {
    recording_ = false;
    processor_.setTracing(false);
    const auto name = "bambi-trace-" + juce::Time::getCurrentTime().formatted("%Y%m%d-%H%M%S") + ".csv";
    const auto file = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile(name);
    status_ = file.replaceWithData(rows_.data(), rows_.size())
                  ? "wrote " + file.getFullPathName() + "  (" + juce::String(frames_) + " frames)"
                  : "could not write " + file.getFullPathName();
    rows_.clear();
    rows_.shrink_to_fit();
}

}  // namespace bambi::ui
