#!/usr/bin/env bash
# One development round on the console: build, link, package, deploy, run,
# collect the logs.
#
#   PS5_HOST=<ip> tools/ps5/dev.sh [seconds] [--eboot-only]
#
# Needs ps5vkctl loaded (tools/ps5.sh payload ...). With --eboot-only only
# eboot.bin is uploaded (assets unchanged since the last full deploy).
# Logs land in ../logs/run-<time>/ and the boot log's tail is printed.
set -euo pipefail
: "${PS5_HOST:?set PS5_HOST=<ps5 ip>}"
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
ps5=$root/../tools/ps5.sh
seconds=${1:-90}
build=$root/build-ps5
title=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["titleId"])' "$root/ps5/sce_sys/param.json")
app=/data/homebrew/$title

ninja -C "$build" > "$build/dev-build.log" 2>&1 || { grep -E 'error' "$build/dev-build.log" | head -20; exit 1; }
"$root/tools/ps5/link.sh" "$build" > "$build/dev-link.log" 2>&1 || { grep -E 'error' "$build/dev-link.log" | head -20; exit 1; }
"$root/tools/ps5/package.sh" "$build" > /dev/null

"$ps5" ctl kill "$title" > /dev/null 2>&1 || true
if [[ ${2:-} == --eboot-only ]]; then
    mkdir -p "$build/eboot-only" && cp "$build/pkg/$title/eboot.bin" "$build/eboot-only/eboot.bin"
    "$ps5" deploy "$build/eboot-only" "$title" > /dev/null
else
    "$ps5" deploy "$build/pkg/$title" > /dev/null
fi

out=$root/../logs/run-$(date +%H%M%S)
mkdir -p "$out"
# Logs: /data/wow_ps once the app's sandbox is elevated, else the app folder.
"$ps5" run "$title" /data/wow_ps/boot_startup.log 'WOWPS_DEV_NEVER_MATCHES' "$seconds" "$out/boot_startup.log" || true
for f in wowps/logs/boot.log wowps/logs/wowps.log; do
    "$ps5" get "/data/wow_ps/$f" "$out/${f##*/}" > /dev/null 2>&1 ||
        "$ps5" get "$app/$f" "$out/${f##*/}" > /dev/null 2>&1 || true
done
echo "logs: $out"
tail -5 "$out/boot.log" 2>/dev/null || tail -5 "$out/boot_startup.log"
