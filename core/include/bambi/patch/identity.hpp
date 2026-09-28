// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace bambi {

/// Instance identity, shared between the state format and the link bus. `session` is shared by
/// every instance in one project -- a host process id can't serve this, since hosts sandbox
/// plugins into several processes -- and `instance` is unique per plugin instance; both are
/// stable across a save/reload.

/// Which plugin an instance is. A document says which product wrote it so a patch from another
/// plugin is refused rather than half-applied, and a version number reads against that plugin's meaning.
enum class Product : std::uint32_t { Unknown = 0, Encoder = 1, Echo = 2, Reverb = 3 };

std::string_view name(Product p);
Product productFromName(std::string_view s);

struct Uuid {
    std::array<std::uint8_t, 16> bytes{};

    static Uuid generate();
    static Uuid nil() { return {}; }

    bool isNil() const;
    std::string toString() const;  ///< 8-4-4-4-12 lowercase hex
    static std::optional<Uuid> parse(std::string_view s);

    friend bool operator==(const Uuid&, const Uuid&) = default;
};

inline constexpr int kNumColours = 6;

struct Identity {
    Uuid session{};
    Uuid instance{};
    std::string label;  ///< user-editable; empty means "fall back to the track name"
    int colour{0};      ///< index into the scene palette, [0, kNumColours)

    static Identity create();  ///< fresh session AND instance — for a genuinely new project
};

}  // namespace bambi
