# pffft — BSD-style FFTPACK licence, see pffft/pffft.h
add_library(pffft STATIC pffft/pffft.c)
target_include_directories(pffft PUBLIC pffft)
target_compile_options(pffft PRIVATE -w)   # third-party: not ours to lint

# dr_wav — public domain / MIT-0, header-only (implementation compiled in core/src/wav.cpp)
add_library(dr_libs INTERFACE)
target_include_directories(dr_libs INTERFACE dr_libs)

# yyjson — MIT
add_library(yyjson STATIC yyjson/yyjson.c)
target_include_directories(yyjson PUBLIC yyjson)
target_compile_options(yyjson PRIVATE -w)

# convhull_3d — MIT, header-only (implementation compiled in convhull_3d/convhull_3d.c)
add_library(convhull_3d STATIC convhull_3d/convhull_3d.c)
target_include_directories(convhull_3d PUBLIC convhull_3d)
target_compile_options(convhull_3d PRIVATE -w)
