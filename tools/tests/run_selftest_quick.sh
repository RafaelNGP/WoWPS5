#!/usr/bin/env bash
# Incremental host build and run of the gameplay self-test (the same report the
# console writes with WOWEE_DEV_SELFTEST), for the tree this script lives in.
#
#   WOWPS_DBC_DIR=<DBFilesClient> tools/tests/run_selftest_quick.sh [--quests]
#
# Objects are kept in ../build-selftest/<tree name>/ so every worktree has its
# own and a rebuild only recompiles what changed (any header change rebuilds
# all). Without --quests the long start-zone quest walk is skipped
# (QUEST_ONLY=none). ABILITY_VERBOSE=1 / QUEST_VERBOSE=1 add detail.
# Exit 0 only if the report has no FAIL line.
set -uo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
dbc=${WOWPS_DBC_DIR:?set WOWPS_DBC_DIR=<client DBFilesClient directory>}
quests=none; [[ ${1:-} == --quests ]] && quests=
out=$root/../build-selftest/$(basename "$root"); mkdir -p "$out"
srcs=(src/game/local_gameplay.cpp src/game/local_melee.cpp src/game/local_services.cpp src/game/local_travel.cpp
      src/game/local_bots.cpp src/game/local_world_catalog.cpp src/game/shapeshift_forms.cpp src/game/local_selftest.cpp
      src/pipeline/dbc_loader.cpp src/core/logger.cpp tools/tests/local_class_items_quests_test.cpp)
headers=0; [[ ! -f $out/.stamp ]] || [[ -n $(find "$root/include" -newer "$out/.stamp" -print -quit) ]] && headers=1
source "$root/tools/ps5/console_lock.sh"
build_lock "host self-test $(basename "$root")"
pids=()
for f in "${srcs[@]}"; do
    o=$out/$(basename "${f%.cpp}").o
    if ((headers)) || [[ ! -f $o || $root/$f -nt $o ]]; then
        "${CXX:-g++}" -std=c++20 -O1 -g -I"$root/include" -I"$root/extern" -I"$root/extern/glm" -c "$root/$f" -o "$o" &
        pids+=($!)
    fi
done
fail=0; for p in "${pids[@]}"; do wait "$p" || fail=1; done
((fail)) && { echo "BUILD FAILED"; exit 2; }
touch "$out/.stamp"
"${CXX:-g++}" "$out"/*.o -pthread -o "$out/selftest" || { echo "LINK FAILED"; exit 2; }
build_unlock
cd "$root" && QUEST_ONLY=$quests timeout 2400 "$out/selftest" assets/local_realm/world.json assets/local_realm/catalog "$dbc" > "$out/run.log" 2>&1
code=$?
grep -av '^\[' "$out/run.log" | grep -E '^(PASS|FAIL|SKIP)|^  ' | cut -c1-240
echo "exit=$code (full log: $out/run.log)"
grep -aq '^FAIL' "$out/run.log" && exit 1
exit $code
