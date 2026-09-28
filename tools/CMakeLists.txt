# SPDX-License-Identifier: GPL-3.0-or-later
#
# Each tool links what it actually uses: bambi-core for the shared foundations, bambi-encode
# on top of it for the ones that encode. That is the point of core being more than one library.
add_executable(bambi-render render/main.cpp)
target_link_libraries(bambi-render PRIVATE bambi-encode)
bambi_warnings(bambi-render)

# Echo's renderer links Echo's engine and nothing of the encoder's, so neither plugin's goldens move
# when the other's engine changes.
add_executable(bambi-echo-render echo-render/main.cpp)
target_link_libraries(bambi-echo-render PRIVATE bambi-echo)
bambi_warnings(bambi-echo-render)
target_include_directories(bambi-echo-render PRIVATE common)

add_executable(bambi-reverb-render reverb-render/main.cpp)
target_link_libraries(bambi-reverb-render PRIVATE bambi-reverb)
bambi_warnings(bambi-reverb-render)
target_include_directories(bambi-reverb-render PRIVATE common)

add_executable(bambi-features features/main.cpp)
target_link_libraries(bambi-features PRIVATE bambi-core)
bambi_warnings(bambi-features)

add_executable(bambi-bench bench/main.cpp)
target_link_libraries(bambi-bench PRIVATE bambi-encode bambi-echo bambi-reverb)
bambi_warnings(bambi-bench)

add_executable(bambi-link link/main.cpp)
target_link_libraries(bambi-link PRIVATE bambi-encode)   # it prints the encoder's parameter names
bambi_warnings(bambi-link)

add_executable(bambi-orders orders/main.cpp)
target_link_libraries(bambi-orders PRIVATE bambi-encode)
bambi_warnings(bambi-orders)

add_executable(bambi-nulltest nulltest/main.cpp)
target_link_libraries(bambi-nulltest PRIVATE bambi-core)
bambi_warnings(bambi-nulltest)

