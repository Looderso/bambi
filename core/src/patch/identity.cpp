// SPDX-License-Identifier: GPL-3.0-or-later
#include "bambi/patch/identity.hpp"

#include <algorithm>
#include <random>

namespace bambi {

std::string_view name(Product p) {
    switch (p) {
        case Product::Encoder: return "encoder";
        case Product::Echo: return "echo";
        case Product::Reverb: return "reverb";
        case Product::Unknown: break;
    }
    return "unknown";
}

Product productFromName(std::string_view s) {
    if (s == "encoder") return Product::Encoder;
    if (s == "echo") return Product::Echo;
    if (s == "reverb") return Product::Reverb;
    return Product::Unknown;
}

namespace {

std::uint64_t random64() {
    // Seeded once per process from the platform entropy source. A plugin can be
    // instantiated dozens of times in a session, so a per-call random_device would be both
    // slow and, on some implementations, degenerate.
    static thread_local std::mt19937_64 rng{[] {
        std::random_device rd;
        return (static_cast<std::uint64_t>(rd()) << 32) ^ rd();
    }()};
    return rng();
}

int hexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

}  // namespace

Uuid Uuid::generate() {
    Uuid u;
    const std::uint64_t a = random64(), b = random64();
    for (int i = 0; i < 8; ++i) {
        u.bytes[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(a >> (8 * i));
        u.bytes[static_cast<std::size_t>(i + 8)] = static_cast<std::uint8_t>(b >> (8 * i));
    }
    u.bytes[6] = static_cast<std::uint8_t>((u.bytes[6] & 0x0F) | 0x40);  // version 4
    u.bytes[8] = static_cast<std::uint8_t>((u.bytes[8] & 0x3F) | 0x80);  // RFC 4122 variant
    return u;
}

bool Uuid::isNil() const {
    return std::all_of(bytes.begin(), bytes.end(), [](std::uint8_t b) { return b == 0; });
}

std::string Uuid::toString() const {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string s;
    s.reserve(36);
    for (int i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) s.push_back('-');
        const std::uint8_t b = bytes[static_cast<std::size_t>(i)];
        s.push_back(kHex[b >> 4]);
        s.push_back(kHex[b & 0x0F]);
    }
    return s;
}

std::optional<Uuid> Uuid::parse(std::string_view s) {
    if (s.size() != 36) return std::nullopt;
    Uuid u;
    std::size_t pos = 0;
    for (int i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) {
            if (s[pos] != '-') return std::nullopt;
            ++pos;
        }
        const int hi = hexVal(s[pos]), lo = hexVal(s[pos + 1]);
        if (hi < 0 || lo < 0) return std::nullopt;
        u.bytes[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>((hi << 4) | lo);
        pos += 2;
    }
    return u;
}

Identity Identity::create() { return Identity{Uuid::generate(), Uuid::generate(), {}, 0}; }

}  // namespace bambi
