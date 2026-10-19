// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wshadow-field-in-constructor"
#endif
#include <pluginterfaces/base/funknown.h>
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

/*  How a host that knows the suite sets one of its plugins without touching the plugin's parameters or its saved
 *  state: an interface only the suite's plugins answer, asked for on the VST3 component by `kRemoteIid`. The plugin
 *  stays the owner of its rules -- the host sends a request and reads back a report, each JSON in the plugin's own
 *  payload, and the plugin grants what its page would. A host checks `remoteKind` and `remoteVersion` against the
 *  payload it speaks and leaves a plugin speaking another alone. The id names these calls: one added, removed or
 *  changed takes a new id.
 *
 *  Message thread on both sides. The object lives as long as the plugin and counts no references.
 */
namespace bambi::remote {

inline constexpr Steinberg::TUID kRemoteIid = INLINE_UID(0x383C05CC, 0x48DC4FF5, 0xBFB158DD, 0x7580653E);

class IRemote : public Steinberg::FUnknown {
public:
    /// Which payload this plugin speaks, "decoder" and the like: zero-terminated, living as long as the plugin.
    virtual const char* PLUGIN_API remoteKind() = 0;
    /// The version of that payload.
    virtual Steinberg::int32 PLUGIN_API remoteVersion() = 0;
    /// The report as JSON, written into `out` with its terminating zero when `capacity` bytes hold it; returns its
    /// length without the zero, so a host asks again with room enough.
    virtual Steinberg::int32 PLUGIN_API remoteReport(char* out, Steinberg::int32 capacity) = 0;
    /// A request as zero-terminated JSON. kResultOk when it was read, whatever was granted: the next report says.
    virtual Steinberg::tresult PLUGIN_API remoteRequest(const char* json) = 0;
};

}  // namespace bambi::remote
