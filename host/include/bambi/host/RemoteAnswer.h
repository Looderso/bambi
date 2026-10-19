// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstring>
#include <functional>
#include <juce_audio_processors/juce_audio_processors.h>
#include <string>
#include <string_view>

#include "bambi/remote/Remote.h"

/*  A plugin's side of the remote interface: handed to JUCE as the plugin's VST3 extensions, it answers the
 *  interface's id on the component, and passes the report and the request to the plugin as text. Owned by the
 *  processor and living as long as it. Message thread.
 */
namespace bambi::host {

class RemoteAnswer final : public juce::VST3ClientExtensions, public remote::IRemote {
public:
    /// `kind` is a literal, living as long as the program.
    RemoteAnswer(const char* kind, int version, std::function<std::string()> report,
                 std::function<void(std::string_view)> request)
        : kind_(kind), version_(version), report_(std::move(report)), request_(std::move(request)) {}

    //  JUCE asks this for every interface the component is asked for: only the remote one is answered here, or this
    //  object would stand in for the component itself.
    int32_t queryIAudioProcessor(const Steinberg::TUID iid, void** obj) override {
        *obj = Steinberg::FUnknownPrivate::iidEqual(iid, remote::kRemoteIid) ? static_cast<remote::IRemote*>(this)
                                                                             : nullptr;
        return *obj != nullptr ? Steinberg::kResultOk : Steinberg::kNoInterface;
    }

    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid, void** obj) override {
        if (Steinberg::FUnknownPrivate::iidEqual(iid, remote::kRemoteIid) ||
            Steinberg::FUnknownPrivate::iidEqual(iid, Steinberg::FUnknown_iid)) {
            *obj = static_cast<remote::IRemote*>(this);
            return Steinberg::kResultOk;
        }
        *obj = nullptr;
        return Steinberg::kNoInterface;
    }
    Steinberg::uint32 PLUGIN_API addRef() override { return 1; }
    Steinberg::uint32 PLUGIN_API release() override { return 1; }

    const char* PLUGIN_API remoteKind() override { return kind_; }
    Steinberg::int32 PLUGIN_API remoteVersion() override { return version_; }
    //  Nothing thrown crosses into the host: a report that fails is a negative length, a request a refusal.
    Steinberg::int32 PLUGIN_API remoteReport(char* out, Steinberg::int32 capacity) override {
        try {
            const std::string text = report_();
            const auto length = static_cast<Steinberg::int32>(text.size());
            if (out != nullptr && capacity > length) std::memcpy(out, text.c_str(), text.size() + 1);
            return length;
        } catch (...) {
            return -1;
        }
    }
    Steinberg::tresult PLUGIN_API remoteRequest(const char* json) override {
        if (json == nullptr) return Steinberg::kInvalidArgument;
        try {
            request_(json);
            return Steinberg::kResultOk;
        } catch (...) {
            return Steinberg::kInternalError;
        }
    }

private:
    const char* kind_;
    int version_;
    std::function<std::string()> report_;
    std::function<void(std::string_view)> request_;
};

}  // namespace bambi::host
