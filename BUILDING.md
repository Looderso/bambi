# Building bambi

bambi builds with CMake and Ninja, in two separate trees: `core/` and `tools/`, which need no JUCE and build in
seconds, and the plugins, which do.

## What you need

- **CMake** 3.21 or newer and **Ninja**.
- **macOS**: Xcode's command-line tools. `brew install cmake ninja ccache`.
- **Windows**: Visual Studio 2022 or its Build Tools, with the C++ workload. Build from a
  "Developer Command Prompt" for x64, which puts the compiler, CMake and Ninja on the path.
- **Linux**: a C++20 compiler, and for the plugins JUCE's dependencies: ALSA, FreeType, fontconfig, X11
  (`libx11-dev libxcomposite-dev libxcursor-dev libxext-dev libxinerama-dev libxrandr-dev libxrender-dev libxi-dev`) and
  Mesa's GL headers.
- **clang-format 23**, to check formatting (`brew install llvm`, or `pipx install clang-format`).

Clone with the JUCE submodule: `git clone --recurse-submodules https://github.com/Looderso/bambi`.

## core and tools

```bash
cmake -B build -G Ninja
cmake --build build
./build/bambi-tests
```

Release is the default. An unoptimised build is several times slower, and a benchmark of one says nothing
about what the plugins cost. Configure with `-DCMAKE_BUILD_TYPE=Debug` to step through something.

`tools/watch.sh` rebuilds and reruns the tests on every save.

## The plugins

```bash
cmake -B build-plugin -G Ninja -DBAMBI_BUILD_PLUGIN=ON
cmake --build build-plugin
```

This builds VST3 and CLAP of each plugin, each plugin's check suite, and the conformance
suite. On macOS the plugins are copied into `~/Library/Audio/Plug-Ins` after building; `-DBAMBI_COPY_PLUGIN=OFF`
turns that off. On Windows, copying into `C:\Program Files\Common Files` needs an administrator prompt.

Linking the plugins with link-time optimisation needs a lot of memory. On a machine with little of it, link
two at a time: `-DCMAKE_JOB_POOLS=link=2 -DCMAKE_JOB_POOL_LINK=link`.

## Checking a change

`tools/verify.sh` runs everything that says a change is sound: the tests, the golden renders, each plugin's
check suite, the picture tools, and the repository's own checks. `tools/verify.sh --core` is the part that
needs no plugin build.

| check | what it catches |
|---|---|
| `tools/golden.sh` | an audio render or a control trace that changed; `--update` re-records them |
| `tools/check-style.sh` | a colour, type size or radius written anywhere but the theme file |
| `tools/check-layers.sh` | an include that runs upward through `core/`'s layers |
| `tools/format.sh --check` | a file not formatted with the repository's `.clang-format`; without `--check` it formats |
| `tools/release/notices.py --check` | a third-party notice that no longer matches what the plugins contain; without `--check` it rewrites them |
| `tools/check-portable.sh` | macOS only: every public header compiles alone, and an x86-64 build renders what the native one does |
| `tools/conformance.sh` | every installed plugin, in both formats, negotiating its buses as a host would |

The golden audio hashes belong to one toolchain, Apple clang on arm64; other systems check the rest.
