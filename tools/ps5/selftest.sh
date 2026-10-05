#!/usr/bin/env bash
# Run the local gameplay self-test (class gear, consumables, start-zone
# quests; src/game/local_selftest.cpp) on the console and collect its report.
#
#   PS5_HOST=<ip> tools/ps5/selftest.sh [--deploy] [--no-quests] [--attempts N]
#                                       [--stall S] [--timeout S]
#   PS5_HOST=<ip> tools/ps5/selftest.sh --detach [options]   # survive the shell
#   tools/ps5/selftest.sh --status                            # progress / result
#
# Turns on WOWEE_DEV_SELFTEST (and WOWEE_DEV_AUTOENTER, so the realm starts
# without input) in the console's config/env.txt for this run only, launches
# the title through ps5vkctl and watches the report. The app writes the report
# line by line, so its growth is a heartbeat: a title that closes, a report
# that stops growing for --stall seconds, or a run longer than --timeout is
# killed and launched again, up to --attempts times. env.txt is restored on
# every exit path. --deploy first builds, links, packages and uploads the app
# (also retried). Exit 0 only on SELFTEST PASS.
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
ps5=$root/../tools/ps5.sh
build=$root/build-ps5
logs=$root/../logs
unit=wowps-selftest

if [[ ${1:-} == --status ]]; then
    systemctl --user is-active "$unit" >/dev/null 2>&1 && echo "state: running ($unit)" || echo "state: not running"
    last=$(ls -dt "$logs"/selftest-* 2>/dev/null | head -1)
    [[ -n $last ]] || { echo "no runs yet"; exit 0; }
    echo "last run: $last"
    tail -5 "$last/runner.log" 2>/dev/null || true
    [[ -f $last/selftest.txt ]] && { echo "--- report"; grep -vE '^  ' "$last/selftest.txt" | tail -8; }
    exit 0
fi
: "${PS5_HOST:?set PS5_HOST=<ps5 ip>}"
if [[ ${1:-} == --detach ]]; then
    shift
    systemctl --user reset-failed "$unit" >/dev/null 2>&1 || true
    systemctl --user stop "$unit" >/dev/null 2>&1 || true
    systemd-run --user --unit="$unit" --working-directory="$root" --setenv=PS5_HOST="$PS5_HOST" \
        "$root/tools/ps5/selftest.sh" "$@"
    echo "detached as $unit; check with: tools/ps5/selftest.sh --status"
    exit 0
fi

title=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["titleId"])' "$root/ps5/sce_sys/param.json")
deploy=0 mode=1 attempts=3 stall=240 timeout=1500
while (($#)); do
    case $1 in
        --deploy) deploy=1 ;;
        --no-quests) mode=2 ;;
        --attempts) attempts=$2; shift ;;
        --stall) stall=$2; shift ;;
        --timeout) timeout=$2; shift ;;
        *) echo "unknown option $1" >&2; exit 2 ;;
    esac
    shift
done
out=$logs/selftest-$(date +%Y%m%d-%H%M%S)
mkdir -p "$out"
exec > >(tee -a "$out/runner.log") 2>&1
log() { echo "[$(date +%H:%M:%S)] $*"; }
retry() {  # retry <tries> <command...>
    local n=$1 i; shift
    for ((i = 1; i <= n; i++)); do "$@" && return 0; log "attempt $i/$n failed: $*"; sleep 5; done
    return 1
}

# After a console reboot ps5vkctl is gone: load it again once (through the
# payload loader, which is all a reboot leaves running).
if ! retry 3 "$ps5" ctl ping > /dev/null; then
    vkctl=$root/../deps/PS5_Vulkan/build/ps5vkctl/ps5vkctl.elf
    log "ps5vkctl does not answer; loading $vkctl"
    [[ -f $vkctl ]] && "$ps5" payload "$vkctl" > /dev/null 2>&1; sleep 5
    retry 3 "$ps5" ctl ping > /dev/null || { log "ps5vkctl does not answer (reload it: tools/ps5.sh payload ...)"; exit 1; }
fi

if ((deploy)); then
    log "build"
    ninja -C "$build" > "$build/dev-build.log" 2>&1 || { grep -E 'error' "$build/dev-build.log" | head -20; exit 1; }
    "$root/tools/ps5/link.sh" "$build" > "$build/dev-link.log" 2>&1 || { grep -E 'error' "$build/dev-link.log" | head -20; exit 1; }
    "$root/tools/ps5/package.sh" "$build" > /dev/null
    "$ps5" ctl kill "$title" > /dev/null 2>&1 || true
    log "deploy"
    retry 3 "$ps5" deploy "$build/pkg/$title" > /dev/null || { log "deploy failed"; exit 1; }
fi

env=/data/wow_ps/wowps/config/env.txt
report=/data/wow_ps/wowps/logs/selftest.txt
retry 3 "$ps5" get "$env" "$out/env.orig.txt" > /dev/null || { log "cannot read $env"; exit 1; }
grep -vE '^(WOWEE_DEV_SELFTEST|WOWEE_DEV_AUTOENTER)=' "$out/env.orig.txt" > "$out/env.txt" || true
printf 'WOWEE_DEV_AUTOENTER=1\nWOWEE_DEV_SELFTEST=%s\n' "$mode" >> "$out/env.txt"
restore() {
    "$ps5" ctl kill "$title" > /dev/null 2>&1 || true
    retry 3 "$ps5" put "$out/env.orig.txt" "$env" > /dev/null && log "env.txt restored" || log "warning: env.txt NOT restored"
}
trap restore EXIT
retry 3 "$ps5" put "$out/env.txt" "$env" > /dev/null

ftp_get() { curl -fsS --ftp-pasv --disable-epsv --connect-timeout 10 --max-time 30 -o "$2" "ftp://$PS5_HOST:${FTP_PORT:-2121}$1" 2>/dev/null; }
result=1
for ((attempt = 1; attempt <= attempts; attempt++)); do
    log "attempt $attempt/$attempts: launch $title"
    curl -fsS --ftp-pasv --disable-epsv --connect-timeout 10 --max-time 30 -o /dev/null -Q "DELE $report" "ftp://$PS5_HOST:${FTP_PORT:-2121}/data/wow_ps/wowps/logs/" 2>/dev/null || true
    rm -f "$out/selftest.txt"
    "$ps5" ctl kill "$title" > /dev/null 2>&1 || true
    sleep 2
    retry 3 "$ps5" ctl launch "$title" || continue
    started=$SECONDS last_size=-1 last_change=$SECONDS why=""
    while :; do
        sleep 5
        if ftp_get "$report" "$out/selftest.part"; then
            mv "$out/selftest.part" "$out/selftest.txt"
            size=$(stat -c %s "$out/selftest.txt")
            if ((size != last_size)); then
                last_size=$size last_change=$SECONDS
                log "heartbeat: $(grep -vE '^  ' "$out/selftest.txt" | tail -1)"
            fi
            grep -q 'SELFTEST PASS' "$out/selftest.txt" && { why=pass; break; }
            grep -q 'SELFTEST FAIL' "$out/selftest.txt" && { why=fail; break; }
        fi
        if ((SECONDS - started > 30)) && ! "$ps5" ctl status 2>/dev/null | grep -q "$title"; then why="title closed"; break; fi
        # Before the first report line the realm is still booting: allow the stall
        # window plus the boot time; afterwards the report must keep growing.
        if ((SECONDS - last_change > stall)); then why="no progress for ${stall}s"; break; fi
        if ((SECONDS - started > timeout)); then why="timeout ${timeout}s"; break; fi
    done
    log "attempt $attempt: $why after $((SECONDS - started)) s"
    "$ps5" get /data/wow_ps/wowps/logs/wowps.log "$out/wowps-attempt$attempt.log" > /dev/null 2>&1 || true
    "$ps5" get /data/wow_ps/boot_startup.log "$out/boot_startup-attempt$attempt.log" > /dev/null 2>&1 || true
    case $why in
        pass) result=0; break ;;
        fail) result=1; break ;;  # a real failure is reported, not retried
        *) log "retrying after: $why"; retry 5 "$ps5" ctl ping > /dev/null || { log "ps5vkctl lost; stop"; break; } ;;
    esac
done
log "logs: $out"
[[ -f $out/selftest.txt ]] && grep -vE '^  ' "$out/selftest.txt" | grep -v '^progress' || log "no self-test report"
((result == 0)) && log "RESULT: PASS" || log "RESULT: FAIL"
exit $result
