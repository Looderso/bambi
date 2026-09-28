// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <span>

#include "bambi/patch/parameters.hpp"
#include "bambi/patch/state.hpp"

/// Where a plugin keeps the parameters the shared modulation engine reads. A manifest binds roles to
/// keys rather than to a plugin's enum, since each plugin's key list is its own and `mod/` sits below
/// every engine in the layering; a role a plugin does not have reads kNoParam, not position zero.
namespace bambi {

/// One LFO's parameters, by position in its plugin's manifest.
struct LfoParams {
    int rate{kNoParam}, sync{kNoParam}, div{kNoParam}, shape{kNoParam}, phase{kNoParam};
    int retrigger{kNoParam}, polarity{kNoParam};
};

struct EnvParams {  ///< one envelope's; its trigger is state, not a parameter, and is not here
    int attack{kNoParam}, decay{kNoParam}, sustain{kNoParam}, release{kNoParam};
    /// A curve per timed stage, and the one polarity knob. Sustain is a level and has none.
    int attackCurve{kNoParam}, decayCurve{kNoParam}, releaseCurve{kNoParam}, polarity{kNoParam};
};

struct ModManifest {
    const ParamManifest* params{nullptr};
    std::span<const LfoParams> lfos;
    std::span<const EnvParams> envelopes;
    std::span<const int> sourceAmount;  ///< the automatable amount of each source slot; kNoParam if none
    std::span<const int> baseTargets;   ///< targets that keep a matrix row whether or not anything drives them
    int globalAmount{kNoParam};

    int amountOf(int slot) const {
        return slot >= 0 && slot < static_cast<int>(sourceAmount.size()) ? sourceAmount[static_cast<std::size_t>(slot)]
                                                                         : kNoParam;
    }

    /// Every role this manifest names resolves to a real parameter; false means a key was misspelled
    /// or a plugin's list is missing something the shared engine needs.
    bool complete() const;
};

/// Built from a plugin's key list, which is the whole of how every plugin builds one: the generator
/// keys and source amounts come from the shared blocks in `patch/blocks.hpp`, so only the base
/// targets -- the ones a plugin exists to move -- are its own argument. Built once, behind each
/// plugin's accessor, on that accessor's first call, which must not be on an audio thread.
inline constexpr std::size_t kMaxBaseTargets = 8;  ///< a cap on base targets rather than a heap

class ModManifestOwner {
public:
    ModManifestOwner(const ParamManifest& p, std::span<const char* const> baseKeys);

    const ModManifest& get() const noexcept { return m_; }

private:
    std::array<LfoParams, 3> lfos_{};
    std::array<EnvParams, 3> envs_{};
    std::array<int, kNumSources> amounts_{};
    std::array<int, kMaxBaseTargets> base_{};
    ModManifest m_;
};

}  // namespace bambi
