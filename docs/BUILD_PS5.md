# Building WoWPS5 for the PS5

WoWPS5 is distributed as **source code only** for now. No prebuilt package is
published: the PS5 binary links third-party runtime components under GPL-3.0
whose terms conflict with this project's non-commercial restriction (see
[the release notes](RELEASE_NOTES_PS5_0.1.0-alpha.md#licensing)). Building it
yourself, for your own console, is fine.

The build runs on Linux (x86-64). It was developed on Fedora/Nobara; other
distributions need the equivalent packages.

## 1. Directory layout

The build expects the dependencies **next to** this repository:

```text
work/
  WoWPS5/            this repository (branch PS5)
  deps/
    PS5_Vulkan/      https://github.com/mihawk-99/PS5_Vulkan   (commit 3f3ee69)
    PS5_PayloadSDK/  https://github.com/mihawk-99/PS5_PayloadSDK
    PS5_Mesa/        https://github.com/mihawk-99/PS5_Mesa      (commit 0b2d6d1)
```

```bash
mkdir -p work/deps && cd work
git clone -b PS5 https://github.com/RafaelNGP/WoWPS5.git
cd deps
git clone https://github.com/mihawk-99/PS5_Vulkan.git     && git -C PS5_Vulkan checkout 3f3ee69
git clone https://github.com/mihawk-99/PS5_PayloadSDK.git
git clone https://github.com/mihawk-99/PS5_Mesa.git       && git -C PS5_Mesa checkout 0b2d6d1
```

Another location works too: export `PS5_VULKAN=/path/to/PS5_Vulkan` before
configuring and linking.

## 2. Host packages

- Compilers and build tools: `clang` and `lld` (LLVM 18 or newer), `llvm-ar`,
  `cmake`, `ninja`, `make`, `python3`, `zip`.
- For Mesa/RADV (Fedora names): `meson glslang ccache clang-devel
  spirv-llvm-translator-devel spirv-tools-devel libclc-devel python3-mako
  python3-markupsafe python3-pyyaml python3-packaging bison flex`.

## 3. Build the PS5 SDK and the RADV driver

```bash
cd work/deps/PS5_Vulkan
tools/setup-native-dependencies.sh     # payload SDK (pinned revision) + zlib -> .deps/native/
tools/build-radv.sh release            # RADV -> .deps/native/radv-release/lib/libvulkan_radeon.ps5.a (~5 min)
```

**Fedora / Nobara:** the clang runtime builtins live under a per-target
directory. If the link step later fails to find
`libclang_rt.builtins-x86_64.a`, add this fallback to PS5_Vulkan's
`tools/radv-link.sh`, right after the line that sets `builtins=`:

```bash
[[ -f $builtins ]] || builtins="$("$compiler" --rtlib=compiler-rt -print-libgcc-file-name)"
```

## 4. Build WoWPS5

```bash
cd work/WoWPS5
cmake -S . -B build-ps5 -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/ps5.toolchain.cmake -DCMAKE_BUILD_TYPE=Release
ninja -C build-ps5            # the game's static libraries (a long build; lower -j if memory is short)
tools/ps5/link.sh build-ps5   # links eboot.bin with RADV and the SDK platform layer
tools/ps5/package.sh build-ps5
```

The result is the app folder **`build-ps5/pkg/PPSA99809/`**. It holds
`eboot.bin`, `sce_sys/`, `sce_module/libc.prx`, `sandbox-elevator.elf`, the
local-world assets, shaders and add-ons. It contains no Blizzard data.

## 5. Install it on the console

Copy `build-ps5/pkg/PPSA99809/` to `/data/homebrew/PPSA99809/`, then follow
[INSTALL_PS5.md](INSTALL_PS5.md): your own WotLK 3.3.5a `Data` directory,
ShadowMountPlus and elfldr.

## Checks (optional)

Host-side tests need your client's extracted `DBFilesClient` directory:

```bash
WOWPS_DBC_DIR=/path/to/DBFilesClient tools/tests/run_selftest_quick.sh --quests   # standalone self-test
SANITIZE=0 tools/tests/run_gameplay_checks.sh local_npc_spell_runtime              # one gameplay suite
```

`tools/ps5/selftest.sh` and `tools/ps5/dev.sh` deploy to a console and run the
self-test there. They are maintainer tools: they call a deploy helper
(`../tools/ps5.sh`: FTP upload and the `ps5vkctl` launcher payload from
PS5_Vulkan) that is not part of this repository.

## Troubleshooting

- **`PS5_SDK=... is not a built payload SDK`**: run step 3, or point
  `PS5_VULKAN` at your PS5_Vulkan checkout.
- **`PS5_RADV=... has no RADV archive`**: `tools/build-radv.sh release` did not
  finish; check its log.
- **The compiler runs out of memory**: build with fewer jobs, e.g.
  `ninja -C build-ps5 -j4`.
