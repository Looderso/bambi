// SPDX-License-Identifier: GPL-3.0-or-later
#include "os.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace bambi::detail {
namespace {

/*  A POSIX name, "/bambi.22.<session>", as a name in this login session's namespace. A backslash is the one
    character a mapping's name may not carry past its namespace, and the names here have none. */
std::string windowsName(const std::string& name) { return "Local\\" + (name.starts_with('/') ? name.substr(1) : name); }

std::string lastError(const char* what) {
    return std::string(what) + " failed: error " + std::to_string(::GetLastError());
}

}  // namespace

bool mapShared(const std::string& name, std::size_t bytes, std::intptr_t& handle, void*& map, std::string& error) {
    map = nullptr;
    handle = -1;

    /*  Creating and sizing are one call, and a segment that exists already is opened at the size it was made
        with: there is no race to lose between two instances loading together. The pages start zeroed. */
    const auto size = static_cast<std::uint64_t>(bytes);
    HANDLE mapping = ::CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, static_cast<DWORD>(size >> 32),
                                          static_cast<DWORD>(size & 0xffffffffull), windowsName(name).c_str());
    if (mapping == nullptr) {
        error = lastError("CreateFileMapping");
        return false;
    }

    void* p = ::MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    if (p == nullptr) {
        error = lastError("MapViewOfFile");
        ::CloseHandle(mapping);
        return false;
    }

    //  Never use more than the segment holds: an older build's smaller segment, opened here, would otherwise
    //  be read past its end.
    MEMORY_BASIC_INFORMATION region{};
    if (::VirtualQuery(p, &region, sizeof region) == 0 || region.RegionSize < bytes) {
        error =
            "shared segment is smaller than this build expects; it was probably created by a different bambi version";
        ::UnmapViewOfFile(p);
        ::CloseHandle(mapping);
        return false;
    }

    map = p;
    handle = reinterpret_cast<std::intptr_t>(mapping);
    return true;
}

void unmapShared(void*& map, std::size_t, std::intptr_t& handle) {
    if (map != nullptr) ::UnmapViewOfFile(map);
    if (handle != -1) ::CloseHandle(reinterpret_cast<HANDLE>(handle));
    map = nullptr;
    handle = -1;
}

void unlinkShared(const std::string&) {}

std::uint64_t monotonicMicros() {
    //  The performance counter is monotonic and system-wide, so two processes comparing these values agree.
    static const std::uint64_t frequency = [] {
        LARGE_INTEGER f{};
        ::QueryPerformanceFrequency(&f);
        return static_cast<std::uint64_t>(f.QuadPart);
    }();
    LARGE_INTEGER now{};
    ::QueryPerformanceCounter(&now);
    const auto ticks = static_cast<std::uint64_t>(now.QuadPart);
    return ticks / frequency * 1000000ull + ticks % frequency * 1000000ull / frequency;
}

}  // namespace bambi::detail
