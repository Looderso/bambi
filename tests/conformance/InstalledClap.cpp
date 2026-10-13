// SPDX-License-Identifier: GPL-3.0-or-later
#include <clap/clap.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

#include "Conformance.h"

#if JUCE_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace bambi::conformance {
namespace {

namespace fs = std::filesystem;

/// Where an installed CLAP is: a bundle on macOS, the library itself elsewhere, in each system's standard folder.
fs::path installedClap(const std::string& name) {
#if JUCE_WINDOWS
    const char* common = std::getenv("CommonProgramFiles");
    return fs::path(common != nullptr ? common : "C:\\Program Files\\Common Files") / "CLAP" / (name + ".clap");
#else
    const char* home = std::getenv("HOME");
    const fs::path base = home != nullptr ? home : "";
#if JUCE_MAC
    return base / "Library/Audio/Plug-Ins/CLAP" / (name + ".clap");
#else
    return base / ".clap" / (name + ".clap");
#endif
#endif
}

const void* hostGetExtension(const clap_host*, const char*) { return nullptr; }
void hostRequestRestart(const clap_host*) {}
void hostRequestProcess(const clap_host*) {}
void hostRequestCallback(const clap_host*) {}

const clap_host kHost = {
    CLAP_VERSION_INIT,
    nullptr,
    "bambi-conformance",
    "bambi",
    "",
    "0.1",
    hostGetExtension,
    hostRequestRestart,
    hostRequestProcess,
    hostRequestCallback,
};

bool isAmbisonic(const char* portType) {
    return portType != nullptr && std::strcmp(portType, CLAP_PORT_AMBISONIC) == 0;
}

void probePlugin(Result& r, const clap_plugin* plugin, Kind kind) {
    const auto check = [&r](bool ok, const std::string& what) { report(r, ok, juce::String(what)); };
    const bool fieldIn = kind != Kind::Source;    // the field arrives on the main input
    const bool fieldOut = kind != Kind::Decoder;  // the field leaves on the main output

    const auto* ports =
        static_cast<const clap_plugin_audio_ports*>(plugin->get_extension(plugin, CLAP_EXT_AUDIO_PORTS));
    check(ports != nullptr, "clap.audio-ports is exposed");
    if (ports == nullptr) return;

    //  A field effect and the decoder take the field on the main input; the encoder takes a stereo source there.
    //  The encoder and the effects return the field on the main output; the decoder returns loudspeaker feeds.
    clap_audio_port_info in0{};
    ports->get(plugin, 0, true, &in0);
    check(isAmbisonic(in0.port_type) == fieldIn, fieldIn
                                                     ? "main input declares CLAP_PORT_AMBISONIC: a field arrives there"
                                                     : "main input is not ambisonic: a source arrives there");
    clap_audio_port_info out0{};
    ports->get(plugin, 0, false, &out0);
    check(isAmbisonic(out0.port_type) == fieldOut, fieldOut
                                                       ? "main output declares CLAP_PORT_AMBISONIC"
                                                       : "main output is not ambisonic: channels of a bus leave there");
    if (fieldOut)
        check(out0.channel_count == 16, "main output is 16 channels (3rd order, the default)");
    else
        check(out0.channel_count == 2 && in0.channel_count == 16,
              "the field arrives as 16 channels (3rd order, the default) and a pair leaves");

    const auto* amb = static_cast<const clap_plugin_ambisonic*>(plugin->get_extension(plugin, CLAP_EXT_AMBISONIC));
    check(amb != nullptr, "clap.ambisonic/3 is exposed");
    if (amb != nullptr) {
        clap_ambisonic_config_t cfg{};
        const bool outputIsAmbisonic = amb->get_config(plugin, false, 0, &cfg);
        if (fieldOut)
            check(outputIsAmbisonic && cfg.ordering == CLAP_AMBISONIC_ORDERING_ACN &&
                      cfg.normalization == CLAP_AMBISONIC_NORMALIZATION_SN3D,
                  "output 0 reports ACN / SN3D");
        else
            check(!outputIsAmbisonic, "output 0 is not reported as ambisonic");
        clap_ambisonic_config_t in{};
        const bool inputIsAmbisonic = amb->get_config(plugin, true, 0, &in);
        if (fieldIn)
            check(inputIsAmbisonic && in.ordering == CLAP_AMBISONIC_ORDERING_ACN &&
                      in.normalization == CLAP_AMBISONIC_NORMALIZATION_SN3D,
                  "input 0 reports ACN / SN3D" + std::string(fieldOut ? " too" : ""));
        else
            check(!inputIsAmbisonic, "input 0 is not reported as ambisonic");
        const clap_ambisonic_config_t acnSn3d{CLAP_AMBISONIC_ORDERING_ACN, CLAP_AMBISONIC_NORMALIZATION_SN3D};
        const clap_ambisonic_config_t fumaMaxn{CLAP_AMBISONIC_ORDERING_FUMA, CLAP_AMBISONIC_NORMALIZATION_MAXN};
        check(amb->is_config_supported(plugin, &acnSn3d), "ACN / SN3D is supported");
        check(!amb->is_config_supported(plugin, &fumaMaxn), "FuMa / MaxN is refused");
    }

    //  Every port configuration, selected and re-read: the ports must follow.
    const auto* configs =
        static_cast<const clap_plugin_audio_ports_config*>(plugin->get_extension(plugin, CLAP_EXT_AUDIO_PORTS_CONFIG));
    check(configs != nullptr, "clap.audio-ports-config is exposed");
    if (configs == nullptr) return;
    const uint32_t n = configs->count(plugin);
    check(n == 10, "ten configurations offered (orders 1 to 10)");
    for (uint32_t i = 0; i < n; ++i) {
        clap_audio_ports_config c{};
        if (!configs->get(plugin, i, &c)) {
            check(false, "get configuration " + std::to_string(i));
            continue;
        }
        //  id equals position, so a host passing either selects the same configuration
        check(c.id == i, "configuration " + std::to_string(i) + " has id " + std::to_string(c.id));
        const bool selected = configs->select(plugin, c.id);
        clap_ambisonic_config_t cfg{};
        clap_audio_port_info after{};
        if (fieldOut) {
            ports->get(plugin, 0, false, &after);
            const bool follows = selected && after.channel_count == c.main_output_channel_count &&
                                 isAmbisonic(after.port_type) && amb != nullptr &&
                                 amb->get_config(plugin, false, 0, &cfg);
            check(follows, std::string("select \"") + c.name + "\" -> output " + std::to_string(after.channel_count) +
                               " ch, ambisonic");
        } else {
            //  a decoder's configurations move the field's order on the input; the output stays the bus it was
            ports->get(plugin, 0, true, &after);
            clap_audio_port_info out{};
            ports->get(plugin, 0, false, &out);
            const bool follows = selected && after.channel_count == c.main_input_channel_count &&
                                 isAmbisonic(after.port_type) && amb != nullptr &&
                                 amb->get_config(plugin, true, 0, &cfg) && out.channel_count == 2 &&
                                 !isAmbisonic(out.port_type);
            check(follows, std::string("select \"") + c.name + "\" -> input " + std::to_string(after.channel_count) +
                               " ch, ambisonic; the output still the pair");
        }
    }
}

}  // namespace

Result probeInstalledClap(const Subject& subject) {
    Result r;
    std::printf("\n%s, the installed CLAP\n", subject.name.c_str());
    const fs::path bundle = installedClap(subject.name);
    fs::path binary;
#if JUCE_MAC
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(bundle / "Contents/MacOS", ec))
        if (e.is_regular_file()) binary = e.path();
#else
    if (fs::is_regular_file(bundle)) binary = bundle;  // the library itself, not a bundle
#endif
    if (binary.empty()) {
        report(r, false, "not installed, so never loaded as a host loads it: " + juce::String(bundle.string()));
        return r;
    }

    //  Never unloaded: plugin frameworks are often unhappy being unloaded, and it proves nothing here.
#if JUCE_WINDOWS
    HMODULE lib = ::LoadLibraryW(binary.c_str());
    const auto* entry =
        lib != nullptr ? reinterpret_cast<const clap_plugin_entry*>(::GetProcAddress(lib, "clap_entry")) : nullptr;
#else
    void* lib = dlopen(binary.c_str(), RTLD_LOCAL | RTLD_NOW);
    const auto* entry = lib != nullptr ? static_cast<const clap_plugin_entry*>(dlsym(lib, "clap_entry")) : nullptr;
#endif
    const bool entered = entry != nullptr && entry->init(bundle.string().c_str());
    report(r, entered, "it loads, and clap_entry initialises");
    if (!entered) return r;

    const auto* factory = static_cast<const clap_plugin_factory*>(entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
    const bool hasPlugin = factory != nullptr && factory->get_plugin_count(factory) > 0;
    report(r, hasPlugin, "its factory offers a plugin");
    if (hasPlugin) {
        const clap_plugin_descriptor* desc = factory->get_plugin_descriptor(factory, 0);
        const clap_plugin* plugin = factory->create_plugin(factory, &kHost, desc->id);
        const bool created = plugin != nullptr && plugin->init(plugin);
        report(r, created, juce::String("it creates and initialises: ") + desc->id);
        if (created) {
            probePlugin(r, plugin, subject.kind);
            plugin->destroy(plugin);
        }
    }
    entry->deinit();
    return r;
}

}  // namespace bambi::conformance
