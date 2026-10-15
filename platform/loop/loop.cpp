// SPDX-License-Identifier: GPL-3.0-or-later
#include "loop.hpp"

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

}  // namespace

bool installed() { return device() != kAudioObjectUnknown; }

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
bool nameChannels(const std::vector<std::string>&) { return false; }
std::string channelName(int) { return {}; }
#endif

}  // namespace bambi::loop
