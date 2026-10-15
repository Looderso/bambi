// SPDX-License-Identifier: GPL-3.0-or-later
#include "loop.hpp"

#include <algorithm>

#ifdef __APPLE__
#include <CoreAudio/CoreAudio.h>
#endif

namespace bambi::loop {

#ifdef __APPLE__
namespace {

AudioObjectID device() {
    CFStringRef uid = CFSTR("bambi_UID");
    AudioObjectID id = kAudioObjectUnknown;
    UInt32 size = sizeof id;
    const AudioObjectPropertyAddress a{kAudioHardwarePropertyTranslateUIDToDevice, kAudioObjectPropertyScopeGlobal,
                                       kAudioObjectPropertyElementMain};
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, sizeof uid, &uid, &size, &id);
    return id;
}

//  Every aggregate device's sub-devices, by uid.
std::vector<std::string> aggregated() {
    std::vector<std::string> uids;
    const AudioObjectPropertyAddress all{kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal,
                                         kAudioObjectPropertyElementMain};
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &all, 0, nullptr, &size) != noErr) return uids;
    std::vector<AudioObjectID> devices(size / sizeof(AudioObjectID));
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &all, 0, nullptr, &size, devices.data()) != noErr)
        return uids;
    for (const AudioObjectID d : devices) {
        UInt32 transport = 0, tsize = sizeof transport;
        const AudioObjectPropertyAddress t{kAudioDevicePropertyTransportType, kAudioObjectPropertyScopeGlobal,
                                           kAudioObjectPropertyElementMain};
        if (AudioObjectGetPropertyData(d, &t, 0, nullptr, &tsize, &transport) != noErr ||
            transport != kAudioDeviceTransportTypeAggregate)
            continue;
        CFArrayRef subs = nullptr;
        UInt32 ssize = sizeof subs;
        const AudioObjectPropertyAddress sub{kAudioAggregateDevicePropertyFullSubDeviceList,
                                             kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
        if (AudioObjectGetPropertyData(d, &sub, 0, nullptr, &ssize, &subs) != noErr || subs == nullptr) continue;
        for (CFIndex i = 0; i < CFArrayGetCount(subs); ++i) {
            const auto uid = static_cast<CFStringRef>(CFArrayGetValueAtIndex(subs, i));
            char buffer[256] = {};
            if (CFStringGetCString(uid, buffer, sizeof buffer, kCFStringEncodingUTF8)) uids.emplace_back(buffer);
        }
        CFRelease(subs);
    }
    return uids;
}

}  // namespace

bool installed() { return device() != kAudioObjectUnknown; }

std::string inAggregate() {
    const auto uids = aggregated();
    const auto holds = [&](const char* uid) { return std::find(uids.begin(), uids.end(), uid) != uids.end(); };
    if (holds("bambi_UID")) return "bambi";
    if (holds("BlackHole16ch_UID")) return "BlackHole 16ch";
    return {};
}

bool nameChannels(const std::vector<std::string>& names) {
    const AudioObjectID d = device();
    if (d == kAudioObjectUnknown) return false;
    CFMutableArrayRef list = CFArrayCreateMutable(nullptr, static_cast<CFIndex>(names.size()), &kCFTypeArrayCallBacks);
    for (const std::string& n : names) {
        CFStringRef str = CFStringCreateWithCString(nullptr, n.c_str(), kCFStringEncodingUTF8);
        CFArrayAppendValue(list, str);
        CFRelease(str);
    }
    const AudioObjectPropertyAddress a{'bnam', kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    CFPropertyListRef value = list;
    const OSStatus status = AudioObjectSetPropertyData(d, &a, 0, nullptr, sizeof value, &value);
    CFRelease(list);
    return status == noErr;
}

std::string channelName(int channel) {
    const AudioObjectID d = device();
    CFStringRef name = nullptr;
    UInt32 size = sizeof name;
    const AudioObjectPropertyAddress a{kAudioObjectPropertyElementName, kAudioObjectPropertyScopeOutput,
                                       static_cast<AudioObjectPropertyElement>(channel)};
    if (d == kAudioObjectUnknown || AudioObjectGetPropertyData(d, &a, 0, nullptr, &size, &name) != noErr ||
        name == nullptr)
        return {};
    char buf[256] = {};
    CFStringGetCString(name, buf, sizeof buf, kCFStringEncodingUTF8);
    CFRelease(name);
    return buf;
}
#else
bool installed() { return false; }
std::string inAggregate() { return {}; }
bool nameChannels(const std::vector<std::string>&) { return false; }
std::string channelName(int) { return {}; }
#endif

}  // namespace bambi::loop
