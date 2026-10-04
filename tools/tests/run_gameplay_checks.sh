#!/usr/bin/env bash
# Host checks for local gameplay rules: the self-test (class gear,
# consumables, start-zone quests) plus the regression suites around them.
#
#   tools/tests/run_gameplay_checks.sh [--suite-timeout S] [suite ...]
#   tools/tests/run_gameplay_checks.sh --detach [options]   # survive the shell
#   tools/tests/run_gameplay_checks.sh --status
#
# Each suite runs under `timeout`; one that hangs or dies from a signal is run
# once more before it counts as failed. WOWPS_DBC_DIR (the client's
# DBFilesClient) is passed to the suites that need it, otherwise they are
# skipped. SANITIZE=1 is honoured when libasan is installed.
set -uo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
logs=$root/../logs
unit=wowps-hostcheck
default_suites=(local_class_items_quests local_inventory local_inventory_0184 local_quest_chain
    local_quest_rewards local_progression local_progression_0173 local_test_characters
    local_warrior_progression local_warrior_stats local_forms local_runes local_pets
    local_mount merchant_authority local_druid_progression local_shaman_progression)

if [[ ${1:-} == --status ]]; then
    systemctl --user is-active "$unit" >/dev/null 2>&1 && echo "state: running ($unit)" || echo "state: not running"
    last=$(ls -dt "$logs"/hostcheck-* 2>/dev/null | head -1)
    [[ -n $last ]] && { echo "last run: $last"; cat "$last/summary.txt" 2>/dev/null; } || echo "no runs yet"
    exit 0
fi
if [[ ${1:-} == --detach ]]; then
    shift
    systemctl --user reset-failed "$unit" >/dev/null 2>&1 || true
    systemctl --user stop "$unit" >/dev/null 2>&1 || true
    systemd-run --user --unit="$unit" --working-directory="$root" \
        ${WOWPS_DBC_DIR:+--setenv=WOWPS_DBC_DIR="$WOWPS_DBC_DIR"} ${SANITIZE:+--setenv=SANITIZE="$SANITIZE"} \
        "$root/tools/tests/run_gameplay_checks.sh" "$@"
    echo "detached as $unit; check with: tools/tests/run_gameplay_checks.sh --status"
    exit 0
fi
suite_timeout=1800 suites=()
while (($#)); do
    case $1 in --suite-timeout) suite_timeout=$2; shift ;; *) suites+=("$1") ;; esac
    shift
done
((${#suites[@]})) || suites=("${default_suites[@]}")
out=$logs/hostcheck-$(date +%Y%m%d-%H%M%S)
mkdir -p "$out"
summary=$out/summary.txt
: > "$summary"
pass=0 fail=0 skip=0
for name in "${suites[@]}"; do
    script=$root/tools/tests/run_${name}_tests.sh
    [[ -f $script ]] || { echo "SKIP $name (no runner)" | tee -a "$summary"; ((skip++)); continue; }
    args=()
    if grep -qE '"\$1"|\$\{1:[-?]|\$1\}' "$script"; then
        [[ -n ${WOWPS_DBC_DIR:-} ]] || { echo "SKIP $name (needs WOWPS_DBC_DIR)" | tee -a "$summary"; ((skip++)); continue; }
        args=("$WOWPS_DBC_DIR")
    fi
    for try in 1 2; do
        start=$SECONDS
        DBC_DIR=${WOWPS_DBC_DIR:-} timeout --kill-after=30 "$suite_timeout" bash "$script" "${args[@]}" > "$out/$name.log" 2>&1
        code=$?
        # 124/137: timed out or killed; >128: died from a signal. Retry once.
        ((code == 124 || code > 128)) && ((try == 1)) && { echo "RETRY $name (exit $code after $((SECONDS - start)) s)" | tee -a "$summary"; continue; }
        break
    done
    groups=$(grep -c '^PASS' "$out/$name.log")
    if ((code == 0)); then
        echo "PASS $name ($groups groups, $((SECONDS - start)) s)" | tee -a "$summary"; ((pass++))
    else
        echo "FAIL $name (exit $code): $(grep -m1 -E 'Assertion|FAIL|error' "$out/$name.log" | cut -c1-200)" | tee -a "$summary"; ((fail++))
    fi
done
echo "TOTAL pass=$pass fail=$fail skip=$skip logs=$out" | tee -a "$summary"
((fail == 0))
