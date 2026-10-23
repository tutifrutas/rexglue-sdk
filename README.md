# ReXGlue SDK — native Linux

The shared static-recompilation SDK for
[Tekken 6](https://github.com/tutifrutas/tekken6-360-native-linux) and
[Tekken Tag Tournament 2](https://github.com/tutifrutas/tekken-tag-tournament-2-360-native-linux)
on Linux. It generates C++ from a user-supplied Xbox 360 executable and provides
the runtime, Vulkan graphics, SDL input/audio and Linux NTSYNC integration.
No Wine or Proton is required. The game ports do not support online play.

This fork is based on [rexglue/rexglue-sdk](https://github.com/rexglue/rexglue-sdk).
Its supported game-port target is Linux x86-64. No game data, generated game
sources, proprietary SDKs, artwork or prebuilt game binaries are included.

## Requirements

- Linux x86-64, Git, Clang 18+, a C++23 standard library, CMake 3.25+, Ninja,
  Meson, Python 3.11+, pkg-config and the usual C build tools.
- Development headers/libraries for X11/XCB (`x11-xcb` in pkg-config) and
  Wayland (`wayland-client`), plus your distribution's SDL3 build prerequisites.
  SDL3 is built from the pinned source submodule; see
  [SDL's Linux dependencies](https://wiki.libsdl.org/SDL3/README-linux).
- A Vulkan-capable graphics driver. Running these game ports requires Vulkan
  1.3, `VK_EXT_fragment_shader_interlock` and the additional renderer feature
  checks. Wayland and the FSI/ROV renderer are the Linux defaults.
- To run games and NTSYNC tests: a kernel providing `/dev/ntsync` and permission
  for your user to open it read/write. There is no silent POSIX fallback.

Dependencies are pinned in `.gitmodules` and CMake. Initialize submodules
recursively. The Vulkan build also fetches pinned
[dxbc-spirv](https://github.com/doitsujin/dxbc-spirv) sources at configure time;
Meson builds that shader compiler as a native library. Network access is needed
for source dependencies, not for gameplay. Qt and KDE are not SDK requirements.

## Build

```sh
git clone --recurse-submodules https://github.com/tutifrutas/rexglue-sdk.git
cd rexglue-sdk
cmake -S . -B build/linux -G Ninja \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_BUILD_TYPE=Release -DREXGLUE_USE_D3D12=OFF \
  -DREXGLUE_USE_VULKAN=ON -DREXGLUE_ENABLE_TRACY=OFF \
  -DREXGLUE_ENABLE_PERF_COUNTERS=OFF
cmake --build build/linux --parallel 8
out/linux-amd64/rexglue --help
```

The game repositories normally build the SDK as a CMake subdirectory using
`-DREXSDK_DIR=/path/to/rexglue-sdk`; a separate SDK installation is not required.
See each game's README for local disc preparation, code generation and launch.
Use the [upstream documentation](https://github.com/rexglue/rexglue-sdk/wiki)
for the general SDK and recompiler interfaces.

## Tests

The full test build needs PowerPC binutils with VMX128 assembler support.
Prebuilt toolchain executables are not bundled. Supply an installed toolchain
with `powerpc-none-elf-as`, `powerpc-none-elf-ld` and `powerpc-none-elf-nm`
(or compatible `powerpc-linux-gnu-*` names). Ordinary distro assemblers may
lack VMX128; verify support for `-mvmx128` before enabling the instruction tests.

```sh
cmake -S . -B build/tests -G Ninja \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_BUILD_TYPE=Release -DREXGLUE_BUILD_TESTS=ON \
  -DPPC_TOOLS_DIR=/path/to/powerpc-binutils/bin \
  -DREXGLUE_USE_VULKAN=ON -DREXGLUE_ENABLE_TRACY=OFF \
  -DREXGLUE_ENABLE_PERF_COUNTERS=OFF
cmake --build build/tests --parallel 8
ctest --test-dir build/tests --output-on-failure
# Optional Python build-tool checks (requires pytest):
python3 -m pytest scripts/tests
```

Tests use source-level fixtures rather than game assets. NTSYNC tests require
the runtime device permissions described above.

## License and credits

The SDK retains its [BSD license](LICENSE), including the ReXGlue and Xenia
copyright notices. Original Linux-port contributions are BSD-3-Clause.
Third-party components retain their own licenses; see [NOTICE](NOTICE) and the
license files in each dependency. The BSD license does not replace GPL, LGPL
or other dependency obligations.

ReXGlue builds on [Xenia](https://github.com/xenia-project/xenia),
[XenonRecomp](https://github.com/hedge-dev/XenonRecomp) and
[rexdex's recompiler](https://github.com/rexdex/recompiler).
Game names and trademarks belong to their respective owners. This independent
project is not affiliated with Bandai Namco or Microsoft and grants no rights
to any game content.
