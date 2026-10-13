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

# zlib — zlib licence; the inflate half only, which libmysofa needs for a SOFA file's compressed datasets.
# Vendored rather than found, so the core builds the same on a bare Windows runner as everywhere else.
add_library(zlib-inflate STATIC zlib/adler32.c zlib/crc32.c zlib/inffast.c zlib/inflate.c zlib/inftrees.c zlib/zutil.c)
target_include_directories(zlib-inflate PUBLIC zlib)
target_compile_options(zlib-inflate PRIVATE -w)

# libmysofa — BSD-3-Clause; the reader only: HDF5 as a SOFA file lays it out, the conventions check, spherical to
# cartesian. The lookup, interpolation, resampling and cache stay upstream.
add_library(libmysofa STATIC
    libmysofa/hrtf/reader.c libmysofa/hrtf/check.c libmysofa/hrtf/spherical.c libmysofa/hrtf/tools.c
    libmysofa/hdf/btree.c libmysofa/hdf/dataobject.c libmysofa/hdf/fractalhead.c libmysofa/hdf/gcol.c
    libmysofa/hdf/gunzip.c libmysofa/hdf/superblock.c)
target_include_directories(libmysofa PUBLIC libmysofa/hrtf)
target_link_libraries(libmysofa PRIVATE zlib-inflate)
target_compile_options(libmysofa PRIVATE -w)

# qrcodegen — MIT; the QR code the decoder draws for its phone page
add_library(qrcodegen STATIC qrcodegen/qrcodegen.cpp)
target_include_directories(qrcodegen PUBLIC qrcodegen)
target_compile_options(qrcodegen PRIVATE -w)

# convhull_3d — MIT, header-only (implementation compiled in convhull_3d/convhull_3d.c)
add_library(convhull_3d STATIC convhull_3d/convhull_3d.c)
target_include_directories(convhull_3d PUBLIC convhull_3d)
target_compile_options(convhull_3d PRIVATE -w)
