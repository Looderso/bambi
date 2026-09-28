// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

/*  What the link bus and the session directory need from the operating system: a named shared-memory
 *  segment, and a clock every process on the machine reads alike. One implementation per OS,
 *  os_posix.cpp and os_windows.cpp; nothing else in core/ includes an OS header. */
namespace bambi::detail {

/*  Opens the named segment, creating it if needed, makes sure it holds at least `bytes`, and maps exactly
 *  that much. Two processes may create it at the same moment, and a segment an older build made may be
 *  smaller than this one expects; mapping past its end would crash the host. On failure `handle` is -1,
 *  `map` is null, `error` says why, and nothing is left open. */
bool mapShared(const std::string& name, std::size_t bytes, std::intptr_t& handle, void*& map, std::string& error);

/// Unmaps and closes what `mapShared` opened; `map` null and `handle` -1 afterwards. Safe on either already clear.
void unmapShared(void*& map, std::size_t bytes, std::intptr_t& handle);

/// Removes the name, so the next `mapShared` creates a fresh segment. On Windows a segment has no name
/// of its own beyond its open handles, and goes when the last one closes: there this does nothing.
void unlinkShared(const std::string& name);

/// Microseconds on a clock that is monotonic and the same in every process on the machine.
std::uint64_t monotonicMicros();

}  // namespace bambi::detail
