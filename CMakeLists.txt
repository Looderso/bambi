# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.21)

# Release by default, on purpose. An unoptimised build of this library is 14x slower on
# the FFT and 5x on the encoder, and benchmarking one by accident produced an entirely
# wrong picture of where the cost was. Configure with -DCMAKE_BUILD_TYPE=Debug when you
# actually need to step through something. Set before project(), which would otherwise pick
# Debug for MSVC; a multi-config generator ignores it.
if(NOT DEFINED CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "")
endif()

# The oldest macOS everything built here runs on -- the Decoder's head-tracking helper needs 14. Unset, every binary
# would need the macOS of the machine that built it. Set before project(), which reads it.
set(CMAKE_OSX_DEPLOYMENT_TARGET 14.0 CACHE STRING "The oldest macOS the build runs on")

project(bambi VERSION 0.1.0 LANGUAGES C CXX)
#  Objective-C, for the one thing that needs it: the decoder's helper app and its launcher, on macOS. A language
#  is enabled at the top, where CMake can set it up; enabled inside a subdirectory it leaves its rules unset.
if(APPLE)
  enable_language(OBJC OBJCXX)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
# Every library ends up inside a plugin, which is a shared library, and on Linux all its code must then be
# position-independent. macOS compiles that way anyway; Windows has no such thing.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
# Nothing a plugin is built from is visible outside it but its entry points, which the formats mark themselves.
# A symbol left visible is one the system's loader may share between the plugins in one process -- every one of
# them carries its own JUCE -- so that one plugin frees what another made.
set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_OBJC_VISIBILITY_PRESET hidden)
set(CMAKE_OBJCXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

# Falling off the end of a function that returns a value is undefined behaviour, and at -O3 the
# compiler is free to run on into whatever comes next. It was a warning nobody read.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-Werror=return-type>)
endif()

# ccache earns its keep once JUCE arrives; harmless before that.
find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
  set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
  message(STATUS "ccache: ${CCACHE_PROGRAM}")
endif()

message(STATUS "build type: ${CMAKE_BUILD_TYPE}")

# A sanitizer build, in a tree of its own: -DBAMBI_SANITIZE=address,undefined (memory errors, undefined behaviour),
# thread (data races) or realtime (an allocation, lock or system call inside a real-time scope; Clang 20 or later).
# Applies to everything built here, JUCE included. Debug symbols, so a report names the line.
set(BAMBI_SANITIZE "" CACHE STRING "Comma-separated sanitizers: address,undefined | thread | realtime")
if(BAMBI_SANITIZE)
  add_compile_options(-fsanitize=${BAMBI_SANITIZE} -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=${BAMBI_SANITIZE})
  #  The real-time marker's compile-time effect analysis would flag every call into code not marked: the runtime
  #  check is the one wanted here.
  if(BAMBI_SANITIZE MATCHES "realtime")
    add_compile_options(-Wno-function-effects)
  endif()
  if(BAMBI_SANITIZE MATCHES "undefined")
    add_compile_options(-fno-sanitize-recover=undefined)
  endif()
  message(STATUS "sanitizers: ${BAMBI_SANITIZE}")
endif()

# Test binaries land in the build root so the documented run command stays short.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

# The warnings every target of ours is built with, spelled once. PEDANTIC adds -Wpedantic, which
# the core libraries take.
function(bambi_warnings target)
  cmake_parse_arguments(W "PEDANTIC" "" "" ${ARGN})
  if(MSVC)
    target_compile_options(${target} PRIVATE /W3)
    return()
  endif()
  target_compile_options(${target} PRIVATE -Wall -Wextra)
  if(W_PEDANTIC)
    target_compile_options(${target} PRIVATE -Wpedantic)
  endif()
endfunction()

# MSVC reads source as the system code page unless told otherwise, and reports __cplusplus as 1997.
# Its C runtime also calls fopen and sscanf unsafe, in favour of functions only it has.
if(MSVC)
  add_compile_options(/utf-8 /Zc:__cplusplus /permissive-)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
endif()

enable_testing()
add_subdirectory(third_party)
add_subdirectory(core)
add_subdirectory(platform)
add_subdirectory(tools)

# The plugin is opt-in. core/ and tools/ build in seconds with no JUCE at all, and that is where
# nearly all development happens. Configure a separate tree for the plugin:
#   cmake -B build-plugin -G Ninja -DBAMBI_BUILD_PLUGIN=ON
option(BAMBI_BUILD_PLUGIN "Build the JUCE plugin: VST3, CLAP, AU, Standalone" OFF)
if(BAMBI_BUILD_PLUGIN)
  add_subdirectory(plugins)
endif()
