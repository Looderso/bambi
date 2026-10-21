// SPDX-License-Identifier: GPL-3.0-or-later
#include "devices.hpp"

#ifdef __APPLE__
#include <CoreAudio/CoreAudio.h>
#include <unistd.h>
#endif

namespace bambi::devices {

#ifdef __APPLE__
namespace {

AudioObjectPropertyAddress at(AudioObjectPropertySelector s,
                              AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal) {
    return {s, scope, kAudioObjectPropertyElementMain};
}

std::string text(CFStringRef s) {
    if (s == nullptr) return {};
    char buffer[512] = {};
    CFStringGetCString(s, buffer, sizeof buffer, kCFStringEncodingUTF8);
    return buffer;
}

std::string stringOf(AudioObjectID d, AudioObjectPropertySelector s) {
    CFStringRef value = nullptr;
    UInt32 size = sizeof(CFStringRef);
    const auto a = at(s);
    if (AudioObjectGetPropertyData(d, &a, 0, nullptr, &size, static_cast<void*>(&value)) != noErr || value == nullptr)
        return {};
    std::string out = text(value);
    CFRelease(value);
    return out;
}

UInt32 numberOf(AudioObjectID d, AudioObjectPropertySelector s, AudioObjectPropertyScope scope) {
    UInt32 value = 0, size = sizeof value;
    const auto a = at(s, scope);
    return AudioObjectGetPropertyData(d, &a, 0, nullptr, &size, &value) == noErr ? value : 0;
}

int channelsOf(AudioObjectID d, AudioObjectPropertyScope scope) {
    const auto a = at(kAudioDevicePropertyStreamConfiguration, scope);
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(d, &a, 0, nullptr, &size) != noErr || size == 0) return 0;
    std::vector<unsigned char> storage(size);
    auto* list = reinterpret_cast<AudioBufferList*>(storage.data());
    if (AudioObjectGetPropertyData(d, &a, 0, nullptr, &size, list) != noErr) return 0;
    int n = 0;
    for (UInt32 i = 0; i < list->mNumberBuffers; ++i) n += static_cast<int>(list->mBuffers[i].mNumberChannels);
    return n;
}

//  The device's latency and safety offset that way, and its first stream's.
int latencyOf(AudioObjectID d, AudioObjectPropertyScope scope) {
    int samples = static_cast<int>(numberOf(d, kAudioDevicePropertyLatency, scope) +
                                   numberOf(d, kAudioDevicePropertySafetyOffset, scope));
    const auto streams = at(kAudioDevicePropertyStreams, scope);
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(d, &streams, 0, nullptr, &size) == noErr && size >= sizeof(AudioStreamID)) {
        std::vector<AudioStreamID> ids(size / sizeof(AudioStreamID));
        if (AudioObjectGetPropertyData(d, &streams, 0, nullptr, &size, ids.data()) == noErr)
            samples += static_cast<int>(numberOf(ids[0], kAudioStreamPropertyLatency, kAudioObjectPropertyScopeGlobal));
    }
    return samples;
}

std::vector<AudioObjectID> all() {
    const auto a = at(kAudioHardwarePropertyDevices);
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &a, 0, nullptr, &size) != noErr) return {};
    std::vector<AudioObjectID> ids(size / sizeof(AudioObjectID));
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, nullptr, &size, ids.data()) != noErr) return {};
    return ids;
}

AudioObjectID byUid(const char* uid) {
    for (const AudioObjectID d : all())
        if (stringOf(d, kAudioDevicePropertyDeviceUID) == uid) return d;
    return kAudioObjectUnknown;
}

CFStringRef cf(const std::string& s) { return CFStringCreateWithCString(nullptr, s.c_str(), kCFStringEncodingUTF8); }

}  // namespace

std::vector<live::AudioDevice> system() {
    std::vector<live::AudioDevice> out;
    for (const AudioObjectID d : all()) {
        const UInt32 transport = numberOf(d, kAudioDevicePropertyTransportType, kAudioObjectPropertyScopeGlobal);
        const std::string uid = stringOf(d, kAudioDevicePropertyDeviceUID);
        if (transport == kAudioDeviceTransportTypeAggregate || uid == kLoopUid || uid.empty()) continue;
        live::AudioDevice device;
        device.uid = uid;
        device.name = stringOf(d, kAudioObjectPropertyName);
        device.outs = channelsOf(d, kAudioObjectPropertyScopeOutput);
        device.ins = channelsOf(d, kAudioObjectPropertyScopeInput);
        device.outLatency = device.outs > 0 ? latencyOf(d, kAudioObjectPropertyScopeOutput) : 0;
        device.inLatency = device.ins > 0 ? latencyOf(d, kAudioObjectPropertyScopeInput) : 0;
        device.wireless = transport == kAudioDeviceTransportTypeBluetooth ||
                          transport == kAudioDeviceTransportTypeBluetoothLE ||
                          transport == kAudioDeviceTransportTypeContinuityCaptureWireless;
        out.push_back(std::move(device));
    }
    return out;
}

int loopChannels() {
    const AudioObjectID d = byUid(kLoopUid);
    return d == kAudioObjectUnknown ? 0 : channelsOf(d, kAudioObjectPropertyScopeOutput);
}

bool Aggregate::create(const std::vector<std::string>& devices, std::string* error) {
    destroy();
    const auto fail = [&](const char* why) {
        if (error != nullptr) *error = why;
        return false;
    };
    if (byUid(kLoopUid) == kAudioObjectUnknown) return fail("bambi's loop device is not installed");
    CFMutableArrayRef subs = CFArrayCreateMutable(nullptr, 0, &kCFTypeArrayCallBacks);
    const std::string clock = devices.empty() ? std::string(kLoopUid) : devices.front();
    std::vector<std::string> members{kLoopUid};
    members.insert(members.end(), devices.begin(), devices.end());
    for (const std::string& uid : members) {
        CFMutableDictionaryRef sub =
            CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        CFStringRef u = cf(uid);
        CFDictionarySetValue(sub, CFSTR(kAudioSubDeviceUIDKey), u);
        const int drift = uid == clock ? 0 : 1;
        CFNumberRef d = CFNumberCreate(nullptr, kCFNumberIntType, &drift);
        CFDictionarySetValue(sub, CFSTR(kAudioSubDeviceDriftCompensationKey), d);
        CFArrayAppendValue(subs, sub);
        CFRelease(d);
        CFRelease(u);
        CFRelease(sub);
    }
    CFMutableDictionaryRef description =
        CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFStringRef uid = cf("bambi_live_host_" + std::to_string(getpid()));
    CFStringRef clockUid = cf(clock);
    const int one = 1, zero = 0;
    CFNumberRef yes = CFNumberCreate(nullptr, kCFNumberIntType, &one);
    CFNumberRef no = CFNumberCreate(nullptr, kCFNumberIntType, &zero);
    CFDictionarySetValue(description, CFSTR(kAudioAggregateDeviceNameKey), CFSTR("bambi live host"));
    CFDictionarySetValue(description, CFSTR(kAudioAggregateDeviceUIDKey), uid);
    CFDictionarySetValue(description, CFSTR(kAudioAggregateDeviceIsPrivateKey), yes);
    CFDictionarySetValue(description, CFSTR(kAudioAggregateDeviceIsStackedKey), no);
    CFDictionarySetValue(description, CFSTR(kAudioAggregateDeviceSubDeviceListKey), subs);
    CFDictionarySetValue(description, CFSTR(kAudioAggregateDeviceMainSubDeviceKey), clockUid);
    AudioObjectID made = kAudioObjectUnknown;
    const OSStatus status = AudioHardwareCreateAggregateDevice(description, &made);
    CFRelease(description);
    CFRelease(subs);
    CFRelease(uid);
    CFRelease(clockUid);
    CFRelease(yes);
    CFRelease(no);
    if (status != noErr || made == kAudioObjectUnknown) return fail("Core Audio would not make the aggregate");
    id_ = made;
    return true;
}

void Aggregate::destroy() {
    if (id_ != 0) AudioHardwareDestroyAggregateDevice(id_);
    id_ = 0;
}
#else
std::vector<live::AudioDevice> system() { return {}; }
int loopChannels() { return 0; }
bool Aggregate::create(const std::vector<std::string>&, std::string* error) {
    if (error != nullptr) *error = "no aggregate devices here";
    return false;
}
void Aggregate::destroy() { id_ = 0; }
#endif

}  // namespace bambi::devices
