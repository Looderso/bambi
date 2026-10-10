// SPDX-License-Identifier: GPL-3.0-or-later
//
//  bambi-live-bridge: sources arriving on a loopback device, placed on the sphere, decoded to binaural
//  and written back onto the same device -- so a host with stereo tracks only, Ableton Live, plays a
//  spatial mix through its own master. macOS only.
//
//    bambi-live-bridge devices
//    bambi-live-bridge aggregate [--out NAME] [--loop NAME]   the "bambi bridge" device for Live to open
//    bambi-live-bridge remove                                  removes it again
//    bambi-live-bridge run [--loop NAME] [--sources N] [--order N] [--rotate DEG_PER_S] [--gain DB]
//                          [--buffer FRAMES] [--probe]
//
//  The loop device's channels, counted from 1: sources on 1..12, the probe's click out on 13 and back
//  on 14, the binaural pair on 15/16. In the aggregate the output device comes first, so Live's master
//  defaults to it and the loop device's outputs are the aggregate's 3..18; its inputs keep their numbers.
//
//  Everything written is held under -12 dBFS, starts silent and fades in over two seconds. The probe
//  writes one -20 dBFS sample a second on 13 and times its return on 14, which measures the host's
//  part of the round trip on the loop device's clock; a latency that creeps over an hour is drift.

#include <CoreAudio/CoreAudio.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#include "bambi/decode/decoder.hpp"
#include "bambi/decode/headmodelfile.hpp"
#include "bambi/math/denormals.hpp"
#include "bambi/math/sh.hpp"
#include "bambi/math/vec3.hpp"

namespace {

constexpr const char* kBridgeName = "bambi bridge";
constexpr const char* kBridgeUid = "org.bambi.live-bridge";
constexpr int kMaxSources = 12;
constexpr int kProbeOut = 12, kProbeBack = 13, kLeft = 14, kRight = 15;  // zero-based on the loop device
constexpr int kLoopChannels = 16;
constexpr int kMaxFrames = 8192;
constexpr float kCeiling = 0.2512f;  // -12 dBFS
constexpr float kProbeLevel = 0.1f;  // -20 dBFS
constexpr float kProbeThreshold = 0.03f;

// ---- Core Audio lookups -------------------------------------------------------------------------------------

AudioObjectPropertyAddress address(AudioObjectPropertySelector s,
                                   AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal) {
    return {s, scope, kAudioObjectPropertyElementMain};
}

std::string toString(CFStringRef s) {
    if (s == nullptr) return {};
    char buf[512] = {};
    CFStringGetCString(s, buf, sizeof buf, kCFStringEncodingUTF8);
    return buf;
}

std::string stringProperty(AudioObjectID id, AudioObjectPropertySelector s) {
    CFStringRef value = nullptr;
    UInt32 size = sizeof value;
    const auto a = address(s);
    if (AudioObjectGetPropertyData(id, &a, 0, nullptr, &size, &value) != noErr) return {};
    std::string out = toString(value);
    CFRelease(value);
    return out;
}

std::vector<AudioObjectID> devices() {
    const auto a = address(kAudioHardwarePropertyDevices);
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &a, 0, nullptr, &size) != noErr) return {};
    std::vector<AudioObjectID> ids(size / sizeof(AudioObjectID));
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, nullptr, &size, ids.data());
    return ids;
}

int channels(AudioObjectID id, AudioObjectPropertyScope scope) {
    const auto a = address(kAudioDevicePropertyStreamConfiguration, scope);
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(id, &a, 0, nullptr, &size) != noErr || size == 0) return 0;
    std::vector<std::byte> raw(size);
    auto* list = reinterpret_cast<AudioBufferList*>(raw.data());
    if (AudioObjectGetPropertyData(id, &a, 0, nullptr, &size, list) != noErr) return 0;
    int n = 0;
    for (UInt32 b = 0; b < list->mNumberBuffers; ++b) n += static_cast<int>(list->mBuffers[b].mNumberChannels);
    return n;
}

template <typename T>
T scalar(AudioObjectID id, AudioObjectPropertySelector s,
         AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal, T fallback = T{}) {
    T value = fallback;
    UInt32 size = sizeof value;
    const auto a = address(s, scope);
    if (AudioObjectGetPropertyData(id, &a, 0, nullptr, &size, &value) != noErr) return fallback;
    return value;
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

AudioObjectID findByName(const std::string& part) {
    for (AudioObjectID id : devices())
        if (lower(stringProperty(id, kAudioObjectPropertyName)).find(lower(part)) != std::string::npos) return id;
    return kAudioObjectUnknown;
}

AudioObjectID findByUid(const std::string& uid) {
    for (AudioObjectID id : devices())
        if (stringProperty(id, kAudioDevicePropertyDeviceUID) == uid) return id;
    return kAudioObjectUnknown;
}

/// The built-in output, whatever the system's default output is: headphones on Bluetooth stay out of it.
AudioObjectID builtInOutput() {
    for (AudioObjectID id : devices())
        if (scalar<UInt32>(id, kAudioDevicePropertyTransportType) == kAudioDeviceTransportTypeBuiltIn &&
            channels(id, kAudioObjectPropertyScopeOutput) > 0)
            return id;
    return kAudioObjectUnknown;
}

std::string option(int argc, char** argv, const char* key, const std::string& fallback) {
    for (int i = 2; i + 1 < argc; ++i)
        if (std::string(argv[i]) == key) return argv[i + 1];
    return fallback;
}

bool flag(int argc, char** argv, const char* key) {
    for (int i = 2; i < argc; ++i)
        if (std::string(argv[i]) == key) return true;
    return false;
}

// ---- devices / aggregate / remove ---------------------------------------------------------------------------

int listDevices() {
    for (AudioObjectID id : devices())
        std::printf("%-40s in %2d  out %2d  %6.0f Hz  %s\n", stringProperty(id, kAudioObjectPropertyName).c_str(),
                    channels(id, kAudioObjectPropertyScopeInput), channels(id, kAudioObjectPropertyScopeOutput),
                    scalar<Float64>(id, kAudioDevicePropertyNominalSampleRate),
                    stringProperty(id, kAudioDevicePropertyDeviceUID).c_str());
    return 0;
}

CFDictionaryRef subDevice(const std::string& uid, bool drift) {
    CFMutableDictionaryRef d =
        CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFStringRef u = CFStringCreateWithCString(nullptr, uid.c_str(), kCFStringEncodingUTF8);
    CFDictionarySetValue(d, CFSTR(kAudioSubDeviceUIDKey), u);
    const int on = drift ? 1 : 0;
    CFNumberRef n = CFNumberCreate(nullptr, kCFNumberIntType, &on);
    CFDictionarySetValue(d, CFSTR(kAudioSubDeviceDriftCompensationKey), n);
    CFRelease(u);
    CFRelease(n);
    return d;
}

int createAggregate(int argc, char** argv) {
    if (findByUid(kBridgeUid) != kAudioObjectUnknown) {
        std::printf("\"%s\" exists already; `remove` first to make it again\n", kBridgeName);
        return 0;
    }
    const std::string outName = option(argc, argv, "--out", "");
    const AudioObjectID out = outName.empty() ? builtInOutput() : findByName(outName);
    const AudioObjectID loop = findByName(option(argc, argv, "--loop", "BlackHole 16ch"));
    if (out == kAudioObjectUnknown || loop == kAudioObjectUnknown) {
        std::fprintf(stderr, "bambi-live-bridge: %s not found; `devices` lists them\n",
                     out == kAudioObjectUnknown ? "the output device" : "the loop device");
        return 1;
    }
    const std::string outUid = stringProperty(out, kAudioDevicePropertyDeviceUID);
    const std::string loopUid = stringProperty(loop, kAudioDevicePropertyDeviceUID);

    CFMutableDictionaryRef desc =
        CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFStringRef name = CFStringCreateWithCString(nullptr, kBridgeName, kCFStringEncodingUTF8);
    CFStringRef uid = CFStringCreateWithCString(nullptr, kBridgeUid, kCFStringEncodingUTF8);
    CFStringRef main = CFStringCreateWithCString(nullptr, outUid.c_str(), kCFStringEncodingUTF8);
    const int zero = 0;
    CFNumberRef no = CFNumberCreate(nullptr, kCFNumberIntType, &zero);
    CFDictionaryRef subs[2] = {subDevice(outUid, false), subDevice(loopUid, true)};
    CFArrayRef list = CFArrayCreate(nullptr, reinterpret_cast<const void**>(subs), 2, &kCFTypeArrayCallBacks);
    CFDictionarySetValue(desc, CFSTR(kAudioAggregateDeviceNameKey), name);
    CFDictionarySetValue(desc, CFSTR(kAudioAggregateDeviceUIDKey), uid);
    CFDictionarySetValue(desc, CFSTR(kAudioAggregateDeviceIsPrivateKey), no);
    CFDictionarySetValue(desc, CFSTR(kAudioAggregateDeviceIsStackedKey), no);
    CFDictionarySetValue(desc, CFSTR(kAudioAggregateDeviceSubDeviceListKey), list);
    CFDictionarySetValue(desc, CFSTR(kAudioAggregateDeviceMainSubDeviceKey), main);

    AudioObjectID aggregate = kAudioObjectUnknown;
    const OSStatus status = AudioHardwareCreateAggregateDevice(desc, &aggregate);
    for (CFTypeRef r : {static_cast<CFTypeRef>(desc), static_cast<CFTypeRef>(name), static_cast<CFTypeRef>(uid),
                        static_cast<CFTypeRef>(main), static_cast<CFTypeRef>(no), static_cast<CFTypeRef>(list),
                        static_cast<CFTypeRef>(subs[0]), static_cast<CFTypeRef>(subs[1])})
        CFRelease(r);
    if (status != noErr) {
        std::fprintf(stderr, "bambi-live-bridge: creating the aggregate failed (%d)\n", static_cast<int>(status));
        return 1;
    }
    //  The sub-devices appear a moment after the aggregate does.
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.5, false);
    std::printf("created \"%s\": %s (clock) + %s (drift-corrected)\n", kBridgeName,
                stringProperty(out, kAudioObjectPropertyName).c_str(),
                stringProperty(loop, kAudioObjectPropertyName).c_str());
    std::printf("  in %d, out %d -- the loop device's outputs are %d..%d here\n",
                channels(aggregate, kAudioObjectPropertyScopeInput),
                channels(aggregate, kAudioObjectPropertyScopeOutput),
                channels(out, kAudioObjectPropertyScopeOutput) + 1,
                channels(out, kAudioObjectPropertyScopeOutput) + channels(loop, kAudioObjectPropertyScopeOutput));
    return 0;
}

int removeAggregate() {
    const AudioObjectID id = findByUid(kBridgeUid);
    if (id == kAudioObjectUnknown) {
        std::printf("no \"%s\" to remove\n", kBridgeName);
        return 0;
    }
    const OSStatus status = AudioHardwareDestroyAggregateDevice(id);
    std::printf(status == noErr ? "removed \"%s\"\n" : "removing \"%s\" failed\n", kBridgeName);
    return status == noErr ? 0 : 1;
}

// ---- run ----------------------------------------------------------------------------------------------------

/// Everything the IO callback touches, allocated before the device starts.
struct Engine {
    bambi::BinauralDecoder decoder;
    int order{3}, ambi{16}, sources{8};
    double rate{48000.0};
    double rotateRadPerSample{0.0};
    float gain{0.5f};
    bool probe{false};

    std::vector<float> amb, ears;         // kMaxFrames x ambi, kMaxFrames x 2
    std::vector<double> from, to, sh;     // sources x ambi gains at the block's start and end; scratch
    double phase{0.0};                    // radians the field has turned
    float fade{0.0f};                     // the output's gain ramp
    std::atomic<float> fadeTarget{1.0f};  // set from the main thread to fade out
    std::atomic<float> fadeStep{0.0f};    // per sample
    int clippedBlocks{0};                 // in a row
    bool muted{false};

    double probeSentAt{-1.0};  // the loop device's output sample time of the last click, or -1
    double nextProbe{0.0};

    std::atomic<float> peakIn[kMaxSources]{};
    std::atomic<float> peakOut{0.0f};
    std::atomic<double> latency{-1.0};  // samples, the last probe's
    std::atomic<int> clipped{0}, tooLong{0}, overloads{0};
    std::atomic<bool> feedback{false};
};

void notePeak(std::atomic<float>& peak, float v) {
    if (v > peak.load(std::memory_order_relaxed)) peak.store(v, std::memory_order_relaxed);
}

/// Channel `c` of a buffer list counted across its buffers: the first sample and the stride, or null.
float* channel(const AudioBufferList* list, int c, int& stride) {
    for (UInt32 b = 0; b < list->mNumberBuffers; ++b) {
        const int n = static_cast<int>(list->mBuffers[b].mNumberChannels);
        if (c < n) {
            stride = n;
            return static_cast<float*>(list->mBuffers[b].mData) + c;
        }
        c -= n;
    }
    return nullptr;
}

void directionGains(Engine& e, double phase, std::vector<double>& out) {
    for (int s = 0; s < e.sources; ++s) {
        const double az = 2.0 * bambi::kPi * s / e.sources + phase;
        bambi::shSN3D(bambi::fromAzEl(az, 0.0), e.order, e.sh);
        std::copy(e.sh.begin(), e.sh.end(), out.begin() + static_cast<std::ptrdiff_t>(s * e.ambi));
    }
}

OSStatus ioProc(AudioObjectID, const AudioTimeStamp*, const AudioBufferList* input, const AudioTimeStamp* inputTime,
                AudioBufferList* output, const AudioTimeStamp* outputTime, void* context) {
    bambi::ScopedFlushDenormals noDenormals;
    auto& e = *static_cast<Engine*>(context);
    for (UInt32 b = 0; b < output->mNumberBuffers; ++b)
        std::fill_n(static_cast<float*>(output->mBuffers[b].mData), output->mBuffers[b].mDataByteSize / sizeof(float),
                    0.0f);
    if (output->mNumberBuffers == 0 || output->mBuffers[0].mNumberChannels == 0) return noErr;
    const int frames =
        static_cast<int>(output->mBuffers[0].mDataByteSize / (sizeof(float) * output->mBuffers[0].mNumberChannels));
    if (frames > kMaxFrames) {
        e.tooLong.fetch_add(1, std::memory_order_relaxed);
        return noErr;
    }

    //  Encode: each source's gains glide from the block's start to its end.
    std::fill_n(e.amb.begin(), frames * e.ambi, 0.0f);
    const double end = e.phase + e.rotateRadPerSample * frames;
    directionGains(e, end, e.to);
    for (int s = 0; s < e.sources; ++s) {
        int stride = 0;
        const float* in = channel(input, s, stride);
        if (in == nullptr) continue;
        const double* g0 = e.from.data() + s * e.ambi;
        const double* g1 = e.to.data() + s * e.ambi;
        float peak = 0.0f;
        for (int f = 0; f < frames; ++f) {
            const float x = in[f * stride];
            peak = std::max(peak, std::abs(x));
            const double t = (f + 1.0) / frames;
            float* a = e.amb.data() + f * e.ambi;
            for (int k = 0; k < e.ambi; ++k) a[k] += static_cast<float>(x * (g0[k] + (g1[k] - g0[k]) * t));
        }
        notePeak(e.peakIn[s], peak);
    }
    e.from.swap(e.to);
    e.phase = std::fmod(end, 2.0 * bambi::kPi);

    e.decoder.process(e.amb.data(), e.ears.data(), frames);

    int strideL = 0, strideR = 0;
    float* left = channel(output, kLeft, strideL);
    float* right = channel(output, kRight, strideR);
    float peak = 0.0f;
    int clipped = 0;
    const float target = e.fadeTarget.load(std::memory_order_relaxed);
    const float step = e.fadeStep.load(std::memory_order_relaxed);
    for (int f = 0; f < frames; ++f) {
        e.fade = e.fade < target ? std::min(target, e.fade + step) : std::max(target, e.fade - step);
        const float g = e.muted ? 0.0f : e.gain * e.fade;
        for (int ear = 0; ear < 2; ++ear) {
            const float v = e.ears[static_cast<std::size_t>(f * 2 + ear)] * g;
            const float held = std::clamp(v, -kCeiling, kCeiling);
            clipped += held != v;
            peak = std::max(peak, std::abs(held));
            float* dst = ear == 0 ? left : right;
            if (dst != nullptr) dst[f * (ear == 0 ? strideL : strideR)] = held;
        }
    }
    notePeak(e.peakOut, peak);
    e.clipped.fetch_add(clipped, std::memory_order_relaxed);
    //  Most of every block held at the ceiling for a quarter of a second is what a loop through the host
    //  looks like.
    e.clippedBlocks = clipped > frames ? e.clippedBlocks + 1 : 0;
    if (e.clippedBlocks * frames > e.rate * 0.25 && !e.muted) {
        e.muted = true;
        e.feedback.store(true, std::memory_order_relaxed);
    }

    if (e.probe) {
        int strideOut = 0, strideBack = 0;
        float* click = channel(output, kProbeOut, strideOut);
        const float* back = channel(input, kProbeBack, strideBack);
        const double outStart = outputTime->mSampleTime, inStart = inputTime->mSampleTime;
        if (click != nullptr && outStart >= e.nextProbe && e.probeSentAt < 0.0) {
            click[0] = kProbeLevel;
            e.probeSentAt = outStart;
            e.nextProbe = outStart + e.rate;
        }
        if (back != nullptr && e.probeSentAt >= 0.0) {
            for (int f = 0; f < frames; ++f)
                if (std::abs(back[f * strideBack]) > kProbeThreshold && inStart + f > e.probeSentAt) {
                    e.latency.store(inStart + f - e.probeSentAt, std::memory_order_relaxed);
                    e.probeSentAt = -1.0;
                    break;
                }
            if (e.probeSentAt >= 0.0 && inStart - e.probeSentAt > e.rate * 0.5) e.probeSentAt = -1.0;  // lost
        }
    }
    return noErr;
}

OSStatus onOverload(AudioObjectID, UInt32, const AudioObjectPropertyAddress*, void* context) {
    static_cast<Engine*>(context)->overloads.fetch_add(1, std::memory_order_relaxed);
    return noErr;
}

std::atomic<bool> stopping{false};

std::string db(float v) {
    if (v < 1e-5f) return "  -inf";
    char buf[16];
    std::snprintf(buf, sizeof buf, "%6.1f", 20.0 * std::log10(v));
    return buf;
}

int run(int argc, char** argv) {
    const AudioObjectID loop = findByName(option(argc, argv, "--loop", "BlackHole 16ch"));
    if (loop == kAudioObjectUnknown) {
        std::fprintf(stderr, "bambi-live-bridge: no loop device; install BlackHole 16ch or name one with --loop\n");
        return 1;
    }
    if (channels(loop, kAudioObjectPropertyScopeInput) < kLoopChannels ||
        channels(loop, kAudioObjectPropertyScopeOutput) < kLoopChannels) {
        std::fprintf(stderr, "bambi-live-bridge: %s has fewer than %d channels each way\n",
                     stringProperty(loop, kAudioObjectPropertyName).c_str(), kLoopChannels);
        return 1;
    }

    static Engine e;
    e.sources = std::clamp(std::stoi(option(argc, argv, "--sources", "8")), 1, kMaxSources);
    e.order = std::clamp(std::stoi(option(argc, argv, "--order", "3")), 1, 7);
    e.ambi = bambi::numChannels(e.order);
    e.rate = scalar<Float64>(loop, kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, 48000.0);
    e.rotateRadPerSample = std::stod(option(argc, argv, "--rotate", "0")) * bambi::kDeg2Rad / e.rate;
    e.gain = static_cast<float>(std::pow(10.0, std::stod(option(argc, argv, "--gain", "-6")) / 20.0));
    e.probe = flag(argc, argv, "--probe");
    e.fadeStep.store(static_cast<float>(1.0 / (2.0 * e.rate)));
    e.amb.assign(static_cast<std::size_t>(kMaxFrames * e.ambi), 0.0f);
    e.ears.assign(static_cast<std::size_t>(kMaxFrames * 2), 0.0f);
    e.from.assign(static_cast<std::size_t>(e.sources * e.ambi), 0.0);
    e.to = e.from;
    e.sh.assign(static_cast<std::size_t>(e.ambi), 0.0);
    directionGains(e, 0.0, e.from);

    std::ifstream file(std::string(BAMBI_CORE_DIR) + "/data/decode/ku100.json");
    const std::string json((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    bambi::HeadModel model;
    std::string error;
    if (!bambi::parseHeadModel(json, model, &error)) {
        std::fprintf(stderr, "bambi-live-bridge: the KU100 head: %s\n", error.c_str());
        return 1;
    }
    bambi::FilterDesign design;
    design.order = e.order;
    design.sampleRate = e.rate;
    if (!e.decoder.prepare(model, design, 64) || !e.decoder.work()) {
        std::fprintf(stderr, "bambi-live-bridge: the binaural decoder did not prepare\n");
        return 1;
    }

    UInt32 buffer = static_cast<UInt32>(std::stoi(option(argc, argv, "--buffer", "128")));
    const auto bufferAddress = address(kAudioDevicePropertyBufferFrameSize);
    AudioObjectSetPropertyData(loop, &bufferAddress, 0, nullptr, sizeof buffer, &buffer);
    buffer = scalar<UInt32>(loop, kAudioDevicePropertyBufferFrameSize);
    const UInt32 inLatency = scalar<UInt32>(loop, kAudioDevicePropertyLatency, kAudioObjectPropertyScopeInput) +
                             scalar<UInt32>(loop, kAudioDevicePropertySafetyOffset, kAudioObjectPropertyScopeInput);
    const UInt32 outLatency = scalar<UInt32>(loop, kAudioDevicePropertyLatency, kAudioObjectPropertyScopeOutput) +
                              scalar<UInt32>(loop, kAudioDevicePropertySafetyOffset, kAudioObjectPropertyScopeOutput);
    const double ms = 1000.0 / e.rate;
    std::printf("%s at %.0f Hz, buffer %u (%.1f ms); device latency in %u, out %u samples; decoder %d\n",
                stringProperty(loop, kAudioObjectPropertyName).c_str(), e.rate, buffer, buffer * ms, inLatency,
                outLatency, e.decoder.latency());
    std::printf("%d sources on 1..%d, order %d, binaural on 15/16%s; ctrl-c stops\n", e.sources, e.sources, e.order,
                e.probe ? ", probe 13 -> 14" : "");

    AudioDeviceIOProcID proc = nullptr;
    if (AudioDeviceCreateIOProcID(loop, ioProc, &e, &proc) != noErr) {
        std::fprintf(stderr, "bambi-live-bridge: cannot open %s\n",
                     stringProperty(loop, kAudioObjectPropertyName).c_str());
        return 1;
    }
    const auto overload = address(kAudioDeviceProcessorOverload);
    AudioObjectAddPropertyListener(loop, &overload, onOverload, &e);
    std::signal(SIGINT, [](int) { stopping.store(true); });
    AudioDeviceStart(loop, proc);

    double lo = 1e30, hi = -1e30;
    while (!stopping.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        std::string in;
        for (int s = 0; s < e.sources; ++s) in += db(e.peakIn[s].exchange(0.0f)) + " ";
        std::printf("in %s| out %s", in.c_str(), db(e.peakOut.exchange(0.0f)).c_str());
        if (e.probe) {
            const double l = e.latency.exchange(-1.0);
            if (l >= 0.0) {
                lo = std::min(lo, l);
                hi = std::max(hi, l);
                std::printf(" | loop %5.0f smp %5.1f ms (%.0f..%.0f) + ours %u -> spatial path ~%.1f ms", l, l * ms, lo,
                            hi, buffer, (l + buffer) * ms);
            } else {
                std::printf(" | loop  --");
            }
        }
        const int c = e.clipped.exchange(0), o = e.overloads.exchange(0);
        if (c > 0) std::printf(" | held %d", c);
        if (o > 0) std::printf(" | OVERLOAD %d", o);
        if (e.tooLong.exchange(0) > 0) std::printf(" | block too long");
        std::printf("\n");
        if (e.feedback.exchange(false))
            std::printf(
                "muted: the output sat at the ceiling for 0.25 s -- a loop through the host? restart to unmute\n");
        std::fflush(stdout);
    }
    e.fadeStep.store(static_cast<float>(1.0 / (0.1 * e.rate)));
    e.fadeTarget.store(0.0f);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    AudioDeviceStop(loop, proc);
    AudioObjectRemovePropertyListener(loop, &overload, onOverload, &e);
    AudioDeviceDestroyIOProcID(loop, proc);
    std::printf("\nstopped\n");
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string command = argc > 1 ? argv[1] : "";
    if (command == "devices") return listDevices();
    if (command == "aggregate") return createAggregate(argc, argv);
    if (command == "remove") return removeAggregate();
    if (command == "run") return run(argc, argv);
    std::fprintf(stderr,
                 "usage: bambi-live-bridge devices | aggregate [--out NAME] [--loop NAME] | remove\n"
                 "       bambi-live-bridge run [--loop NAME] [--sources N] [--order N] [--rotate DEG_PER_S]\n"
                 "                             [--gain DB] [--buffer FRAMES] [--probe]\n");
    return 2;
}
