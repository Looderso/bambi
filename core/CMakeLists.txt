# SPDX-License-Identifier: GPL-3.0-or-later
#
# core/ carries no JUCE dependency, by design: everything musical in this project is pure
# computation over buffers and must stay testable in seconds without a plugin.
#
# It is more than one library. The shared foundations are bambi-core; each plugin's ENGINE is its
# own library beside them, so the encoder never links Reverb's feedback delay network and the
# boundary is expressed in the build rather than in a folder convention.
#
#   bambi-core     math/ dsp/ path/ mod/ patch/ link/ scene/ io/   -- shared by every plugin
#   bambi-encode   encode/                                         -- the encoder's engine
#   (bambi-echo, bambi-reverb follow, as their engines are written)
#
# The headers are layered and the layering is acyclic: math and the header-only primitives depend
# on nothing, and nothing in a lower layer includes a higher one.

add_library(bambi-core STATIC
  src/math/legendre.cpp
  src/math/sh.cpp
  src/math/shrotation.cpp
  src/math/sphere.cpp

  src/dsp/features.cpp
  src/dsp/biquad.cpp
  src/dsp/field.cpp
  src/dsp/fielddelay.cpp
  src/dsp/fft.cpp

  src/region/shape.cpp
  src/region/projection.cpp

  src/path/authoring.cpp
  src/path/generator.cpp
  src/path/trajectory.cpp

  src/mod/manifest.cpp
  src/mod/matrix.cpp
  src/mod/modulation.cpp
  src/mod/regionclip.cpp
  src/mod/sources.cpp

  src/patch/identity.cpp
  src/patch/json.cpp
  src/patch/parameters.cpp
  src/patch/presets.cpp
  src/patch/readparams.cpp
  src/patch/state.cpp
  src/patch/undo.cpp

  src/link/directory.cpp
  src/link/link.cpp
  $<IF:$<PLATFORM_ID:Windows>,src/link/os_windows.cpp,src/link/os_posix.cpp>

  src/scene/view.cpp
  src/io/wav.cpp

  src/scene/energy.cpp)
target_include_directories(bambi-core PUBLIC include)
target_link_libraries(bambi-core PRIVATE pffft yyjson dr_libs)
bambi_warnings(bambi-core PEDANTIC)

# ---- the encoder's engine ---------------------------------------------------------------------
add_library(bambi-encode STATIC
  src/encode/params.cpp
  src/encode/pathparams.cpp
  src/encode/encoder.cpp
  src/encode/control.cpp
)
target_link_libraries(bambi-encode PUBLIC bambi-core)
bambi_warnings(bambi-encode PEDANTIC)

# ---- Echo's engine ----------------------------------------------------------------------------
add_library(bambi-echo STATIC
  src/echo/warp.cpp
  src/echo/pass.cpp
  src/echo/timing.cpp
  src/echo/engine.cpp
  src/echo/control.cpp
  src/echo/params.cpp
  src/echo/ping.cpp
  src/echo/patterns.cpp
)
target_link_libraries(bambi-echo PUBLIC bambi-core)
bambi_warnings(bambi-echo PEDANTIC)

# ---- Reverb's engine --------------------------------------------------------------------------
add_library(bambi-reverb STATIC
  src/reverb/room.cpp
  src/reverb/tailplan.cpp
  src/reverb/tail.cpp
  src/reverb/images.cpp
  src/reverb/early.cpp
  src/reverb/engine.cpp
  src/reverb/control.cpp
  src/reverb/presets.cpp
  src/reverb/params.cpp
)
target_link_libraries(bambi-reverb PUBLIC bambi-core)
bambi_warnings(bambi-reverb PEDANTIC)

# ---- one test binary over all of them ----------------------------------------------------------
add_executable(bambi-tests
  tests/main.cpp

  tests/math/test_vec3.cpp
  tests/math/test_legendre.cpp
  tests/math/test_sh.cpp
  tests/math/test_shrotation.cpp

  tests/dsp/test_features.cpp
  tests/dsp/test_biquad.cpp
  tests/dsp/test_field.cpp
  tests/dsp/test_fielddelay.cpp
  tests/dsp/test_fft.cpp

  tests/region/test_shape.cpp
  tests/region/test_projection.cpp
  tests/region/test_weights.cpp

  tests/path/test_authoring.cpp
  tests/path/test_trajectory.cpp

  tests/mod/test_matrix.cpp
  tests/mod/test_manifest.cpp
  tests/mod/test_beatclock.cpp
  tests/mod/test_modulation.cpp
  tests/mod/test_regionclip.cpp
  tests/mod/test_sources.cpp

  tests/patch/test_identity.cpp
  tests/patch/test_json.cpp
  tests/patch/test_blocks.cpp
  tests/patch/test_parameters.cpp
  tests/patch/test_presets.cpp
  tests/patch/test_readparams.cpp
  tests/patch/test_noalloc.cpp
  tests/patch/test_state.cpp
  tests/patch/test_state_products.cpp
  tests/patch/test_undo.cpp

  tests/link/test_directory.cpp
  tests/link/test_latest.cpp
  tests/link/test_link.cpp
  tests/link/test_spsc.cpp

  tests/scene/test_view.cpp
  tests/scene/test_energy.cpp
  tests/io/test_wav.cpp

  tests/encode/test_encoder.cpp
  tests/encode/test_control.cpp

  tests/echo/test_warp.cpp
  tests/echo/test_pass.cpp
  tests/echo/test_timing.cpp
  tests/echo/test_engine.cpp
  tests/echo/test_echo_control.cpp
  tests/echo/test_echo_params.cpp
  tests/echo/test_patterns.cpp
  tests/echo/test_ping.cpp

  tests/reverb/test_room.cpp
  tests/reverb/test_tailplan.cpp
  tests/reverb/test_tail.cpp
  tests/reverb/test_images.cpp
  tests/reverb/test_early.cpp
  tests/reverb/test_reverb_control.cpp
  tests/reverb/test_reverb_engine.cpp
  tests/reverb/test_reverb_regions.cpp
  tests/reverb/test_reverb_params.cpp
)
target_link_libraries(bambi-tests PRIVATE bambi-encode bambi-echo bambi-reverb)
target_include_directories(bambi-tests PRIVATE tests)
# doctest prints a string_view through <<, which MSVC's standard library defines only in <ostream>
if(MSVC)
  target_compile_options(bambi-tests PRIVATE /FIostream)
endif()
bambi_warnings(bambi-tests)

add_test(NAME core COMMAND bambi-tests)
