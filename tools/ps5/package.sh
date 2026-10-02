#!/usr/bin/env bash
# Stage the PS5 app folder (ShadowMountPlus directory title) from a build.
#
#   tools/ps5/package.sh <build-dir> [out-dir]     -> <out-dir>/<TITLE_ID>/
#
# Layout: eboot.bin, sce_sys/{param.json,icon0.png}, sce_module/libc.prx (the
# boilerplate clean-room runtime), and what the client reads from /app0:
# assets/, expansions/ + opcodes/ (the client's protocol tables) and addons/.
# The user's WotLK MPQs are NOT staged: they go to /app0/Data on the console.
set -euo pipefail

build=$(cd "${1:?usage: package.sh <build-dir> [out-dir]}" && pwd)
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
vk=${PS5_VULKAN:-$(cd "$root/../deps/PS5_Vulkan" && pwd)}
param=$root/ps5/sce_sys/param.json
title=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["titleId"])' "$param")
out=${2:-$build/pkg}
app=$out/$title

[[ -f $build/eboot.bin ]] || { echo "missing $build/eboot.bin (run tools/ps5/link.sh)" >&2; exit 2; }
(cd "$vk/runtime" && sha256sum --check --strict --quiet libc.prx.sha256)
python3 "$root/tools/ps4/verify_shaders.py"

rm -rf -- "$app"
mkdir -p "$app/sce_sys" "$app/sce_module" "$app/assets/shaders"
cp "$build/eboot.bin" "$app/eboot.bin"
cp "$param" "$app/sce_sys/param.json"
cp "$root/ps5/sce_sys/icon0.png" "$app/sce_sys/icon0.png"
cp "$vk/runtime/libc.prx" "$app/sce_module/libc.prx"

cp -r "$root/assets/local_realm" "$app/assets/local_realm"
cp "$root"/assets/shaders/*.spv "$app/assets/shaders/"
[[ -d $root/assets/textures ]] && cp -r "$root/assets/textures" "$app/assets/textures"
for f in grass_biomes.json krayonload.png krayonsignin.png WoWPS.png orgrimmar-entrance.png; do
    [[ -f $root/assets/$f ]] && cp "$root/assets/$f" "$app/assets/$f"
done
# The tables the PS4 build mirrors from /app0/Data into its data root at
# first launch: on the PS5 the data root is /app0, so they are staged there
# and /app0/Data is left to the user's MPQs.
cp -r "$root/Data/." "$app/"
cp -r "$root/addons" "$app/addons"
cmake -DDIR="$app/addons" -P "$root/cmake/strip_saved_vars.cmake"

echo "app: $app ($(du -sh "$app" | cut -f1))"
