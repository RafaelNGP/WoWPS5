#!/usr/bin/env bash
# One full validation round, detached from the shell so a lost session or a
# closed terminal does not stop it:
#   1. host: gameplay self-test (+ client DBC) and the regression suites,
#   2. console: build, deploy and the self-test (with relaunch on hangs),
#   3. console: in-world frame-time measurement (play configuration).
#
#   PS5_HOST=<ip> WOWPS_DBC_DIR=<DBFilesClient> tools/ps5/round.sh [--no-perf] [suite ...]
#   tools/ps5/round.sh --status
#
# The summary is written to ../logs/round-<time>/summary.txt; exit 0 only if
# every step passed.
set -uo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
logs=$root/../logs
unit=wowps-round
default_suites=(local_class_items_quests local_quest_chain local_quest_rewards local_inventory local_mount
    local_pet_runtime local_armor_snapshot_codec local_melee_special local_progression local_warrior_progression
    local_druid_progression local_shaman_progression local_stormstrike_runtime local_feral_runtime
    local_bloodthirst_runtime local_npc_spell_runtime local_harmful_view_codec local_party_lan local_durability_repair)

if [[ ${1:-} == --status ]]; then
    systemctl --user is-active "$unit" >/dev/null 2>&1 && echo "state: running ($unit)" || echo "state: not running"
    last=$(ls -dt "$logs"/round-2* 2>/dev/null | head -1)
    [[ -n $last ]] && { echo "last round: $last"; cat "$last/summary.txt" 2>/dev/null; tail -3 "$last/round.log" 2>/dev/null; }
    exit 0
fi
: "${PS5_HOST:?set PS5_HOST=<ps5 ip>}"
if [[ ${ROUND_DETACHED:-0} != 1 ]]; then
    if systemctl --user is-active -q "$unit"; then echo "a round is already running ($unit); check with: tools/ps5/round.sh --status"; exit 1; fi
    systemctl --user reset-failed "$unit" >/dev/null 2>&1 || true
    systemd-run --user --unit="$unit" --working-directory="$root" --setenv=ROUND_DETACHED=1 \
        --setenv=PS5_HOST="$PS5_HOST" ${WOWPS_DBC_DIR:+--setenv=WOWPS_DBC_DIR="$WOWPS_DBC_DIR"} \
        "$root/tools/ps5/round.sh" "$@" >/dev/null || { echo "could not start $unit"; exit 1; }
    echo "round started as $unit; check with: tools/ps5/round.sh --status"
    exit 0
fi
perf=1 suites=()
for a in "$@"; do case $a in --no-perf) perf=0 ;; *) suites+=("$a") ;; esac; done
((${#suites[@]})) || suites=("${default_suites[@]}")
out=$logs/round-$(date +%Y%m%d-%H%M%S)
mkdir -p "$out"
exec > >(tee -a "$out/round.log") 2>&1
summary=$out/summary.txt
step() { echo "$1" | tee -a "$summary"; }
status=0

# Host and console run side by side: the build host is idle while the
# console works and the other way round.
"$root/tools/tests/run_gameplay_checks.sh" "${suites[@]}" > "$out/host.txt" 2>&1 &
host_pid=$!
"$root/tools/ps5/selftest.sh" --deploy > "$out/selftest.txt" 2>&1
selftest=$?
step "$( ((selftest == 0)) && echo PASS || echo FAIL ) console self-test ($(grep -m1 -o 'SELFTEST [A-Z]* seconds=[0-9.]*' "$out/selftest.txt" || echo 'no report'))"
((selftest == 0)) || status=1
if ((perf)); then
    "$root/tools/ps5/perf.sh" 90 > "$out/perf.txt" 2>&1
    step "INFO console perf: $(grep -m1 SUMMARY "$out/perf.txt" || echo 'no measurement')"
fi
wait $host_pid; host=$?
step "$( ((host == 0)) && echo PASS || echo FAIL ) host suites: $(grep TOTAL "$out/host.txt" | tail -1)"
grep -E '^(FAIL|RETRY)' "$out/host.txt" | sed 's/^/    /' >> "$summary"
((host == 0)) || status=1
step "ROUND $( ((status == 0)) && echo PASS || echo FAIL ) logs=$out"
exit $status
