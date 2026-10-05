# Sourced by the console scripts: one agent (or person) uses the PS5 at a time.
#
#   source tools/ps5/console_lock.sh; ps5_lock "<what>"
#
# Takes an exclusive flock on ../coordination/ps5.lock (shared by every
# worktree of the workspace) for the rest of the calling script, waiting up to
# PS5_LOCK_WAIT seconds (default 2 h). The holder is written beside it as
# ps5.lock.holder (agent, tree, what, since) so a waiting agent can see who
# has the console. AGENT_NAME names the agent (default: the tree's name).
ps5_lock() {
    local dir; dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)/../coordination
    mkdir -p "$dir"
    exec 9>"$dir/ps5.lock"
    if ! flock -n 9; then
        echo "[$(date +%H:%M:%S)] PS5 busy: $(cat "$dir/ps5.lock.holder" 2>/dev/null || echo unknown); waiting"
        flock -w "${PS5_LOCK_WAIT:-7200}" 9 || { echo "PS5 lock not acquired"; exit 3; }
    fi
    echo "${AGENT_NAME:-$(basename "$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)")} $(basename "$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)") ${1:-console} since $(date '+%F %T') pid $$" > "$dir/ps5.lock.holder"
}
