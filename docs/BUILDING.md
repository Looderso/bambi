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
- **lychee**, to check the docs' links (`brew install lychee`), and **pre-commit** for the commit hooks
  (`brew install pre-commit`, or `pipx install pre-commit`; `prek` reads the same file).

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

On macOS the Decoder's build also makes its helper, `plugins/decoder/Helper/`, a small app that reads AirPods
through CoreMotion and serves the phone page; `Helper/nest.sh` nests it into both bundles after they are built and
copied, and signs the bundles again. The other platforms build the Decoder without it, and its AirPods and phone
tabs are not offered there. Two more tools come with the Decoder: `tools/head` turns a measured head (`.bin` or
SOFA) into the head file the plugin ships, and `tools/matrix/jconvolver-to-sofa.py` (run with
`uv run --with netCDF4 --with numpy`) turns a jconvolver filter set into the SOFA FIR-E matrix the Decoder loads.

Linking the plugins with link-time optimisation needs a lot of memory. On a machine with little of it, link
two at a time: `-DCMAKE_JOB_POOLS=link=2 -DCMAKE_JOB_POOL_LINK=link`. Or leave it out with `-DBAMBI_LTO=OFF`:
each plugin then links in seconds instead of a minute or more, which is what CI does for a pull request. A release is
always built with it.

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
| `tools/check-docs.sh` | a dead link or anchor (lychee), a decision or repository path that no longer exists, a page past its size budget |
| `tools/check-portable.sh` | macOS only: every public header compiles alone, and an x86-64 build renders what the native one does |
| `tools/conformance.sh` | every installed plugin, in both formats, negotiating its buses as a host would |

**Sanitizers** run in a build tree of their own: `cmake -B build-asan -G Ninja -DBAMBI_SANITIZE=address,undefined`
(memory errors, undefined behaviour), `thread` (data races) or `realtime` (an allocation, lock or system call inside
`processBlock`; needs Clang 20 or later, e.g. `brew install llvm` and `-DCMAKE_CXX_COMPILER=$(brew --prefix llvm)/bin/clang++`).
`tools/validate-plugins.sh` runs pluginval over every built VST3 and clap-validator over every CLAP.

**clang-tidy** reads `.clang-tidy`. Checking all of the code, as the nightly run does, needs macOS (the live host's
device layer is CoreAudio) and the Decoder's generated resources, which its sources include:

```bash
llvm=$(brew --prefix llvm)/bin
cmake -B build-tidy -G Ninja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DBAMBI_BUILD_PLUGIN=ON -DBAMBI_BUILD_HOST=ON \
  -DBAMBI_COPY_PLUGIN=OFF -DCMAKE_C_COMPILER=$llvm/clang -DCMAKE_CXX_COMPILER=$llvm/clang++
cmake --build build-tidy --target bambi-decoder-data
$llvm/run-clang-tidy -p build-tidy -clang-tidy-binary $llvm/clang-tidy \
  '^(?!.*/third_party/)(?!.*/build).*/(core|host|ui|editor|plugins|apps|platform|tools|tests)/.*\.(cpp|mm)$'
```

On another system, a core-only build with `-DCMAKE_EXPORT_COMPILE_COMMANDS=ON` checks core, its tests and the tools.

**Commit hooks** run the fast C++ checks — formatting, theme roles, layers — on what is staged, and the
headers guard before a push: `pre-commit install --hook-type pre-commit --hook-type pre-push`, once per checkout.

The golden audio hashes belong to one toolchain, Apple clang on arm64; other systems check the rest.
