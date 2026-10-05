#!/usr/bin/env bash
# Measure in-world frame times on the console with a play configuration
# (no development screenshots) and print the [WORLD_PERF] summary.
#
#   PS5_HOST=<ip> tools/ps5/perf.sh [seconds-in-world] [extra ENV=VALUE ...]
#
# env.txt is replaced for the run (auto-enter, plus any extra variables) and
# restored afterwards. Logs land in ../logs/perf-<time>/.
set -uo pipefail
: "${PS5_HOST:?set PS5_HOST=<ps5 ip>}"
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
ps5=$root/../tools/ps5.sh
title=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["titleId"])' "$root/ps5/sce_sys/param.json")
seconds=${1:-90}; shift || true
out=$root/../logs/perf-$(date +%Y%m%d-%H%M%S)
mkdir -p "$out"
env=/data/wow_ps/wowps/config/env.txt
log() { echo "[$(date +%H:%M:%S)] $*"; }
source "$root/tools/ps5/console_lock.sh"; ps5_lock "perf"

"$ps5" get "$env" "$out/env.orig.txt" > /dev/null || { log "cannot read env.txt"; exit 1; }
grep -vE '^WOWEE_DEV_' "$out/env.orig.txt" > "$out/env.txt" || true
echo "WOWEE_DEV_AUTOENTER=1" >> "$out/env.txt"
for kv in "$@"; do echo "$kv" >> "$out/env.txt"; done
restore() { "$ps5" ctl kill "$title" > /dev/null 2>&1; "$ps5" put "$out/env.orig.txt" "$env" > /dev/null && log "env.txt restored"; }
trap restore EXIT
"$ps5" put "$out/env.txt" "$env" > /dev/null
"$ps5" ctl kill "$title" > /dev/null 2>&1; sleep 2
"$ps5" ctl launch "$title" > /dev/null || { log "launch failed"; exit 1; }
entered=0
for i in $(seq 1 60); do
    sleep 5
    "$ps5" get /data/wow_ps/wowps/logs/wowps.log "$out/wowps.log" > /dev/null 2>&1 || continue
    grep -aq "entering the world as" "$out/wowps.log" && { entered=1; break; }
done
((entered)) || { log "never entered the world"; exit 1; }
log "in world; measuring ${seconds}s"
sleep "$seconds"
"$ps5" get /data/wow_ps/wowps/logs/wowps.log "$out/wowps.log" > /dev/null 2>&1
grep -a "WORLD_PERF\|frame budget\|stall" "$out/wowps.log" | sed 's/^\[[^]]*\] //' | cut -c1-200 | tail -8
python3 - "$out/wowps.log" <<'PY'
import re, sys, statistics
rows = [l for l in open(sys.argv[1], errors='replace') if '[WORLD_PERF]' in l]
rows = rows[2:] or rows  # skip the loading-screen windows
get = lambda key: [float(m.group(1)) for l in rows for m in [re.search(key + r'=([0-9.]+)', l)] if m]
fps, mx, p99 = get('loopFps'), get('frameMaxMs'), get('frameP99Ms')
if fps:
    print(f"SUMMARY windows={len(fps)} fps mean={statistics.mean(fps):.1f} min={min(fps):.1f} "
          f"frameMax mean={statistics.mean(mx):.0f}ms worst={max(mx):.0f}ms p99 mean={statistics.mean(p99):.1f}ms")
PY
log "logs: $out"
