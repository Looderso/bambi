# SPDX-License-Identifier: GPL-3.0-or-later
#
# What talks to the operating system rather than to audio, for the stand-in host and the host: the loop device's
# names, and the local line server the Live device speaks to. Not core's: core speaks to no platform.
add_library(bambi-loop STATIC loop/loop.cpp)
target_include_directories(bambi-loop PUBLIC loop)
if(APPLE)
  target_link_libraries(bambi-loop PRIVATE "-framework CoreAudio" "-framework CoreFoundation")
endif()
bambi_warnings(bambi-loop)

# POSIX sockets.
if(UNIX)
  add_library(bambi-net STATIC net/lines.cpp)
  target_include_directories(bambi-net PUBLIC net)
  bambi_warnings(bambi-net)
endif()
