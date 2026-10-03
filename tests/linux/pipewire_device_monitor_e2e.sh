#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
source_path=${1:-"$repo_root/build/clearmic-pipewire-test-source"}
monitor_path=${2:-"$repo_root/build/clearmic-pipewire-device-monitor-test"}
if [[ "$source_path" != /* ]]; then source_path="$repo_root/$source_path"; fi
if [[ "$monitor_path" != /* ]]; then monitor_path="$repo_root/$monitor_path"; fi
for executable in "$source_path" "$monitor_path"; do
    if [[ ! -x "$executable" ]]; then
        echo "PipeWire device monitor test executable not found: $executable" >&2
        exit 2
    fi
done
for command in pipewire wireplumber pw-cli dbus-run-session; do
    if ! command -v "$command" >/dev/null; then
        echo "PipeWire device monitor test requires '$command'" >&2
        exit 2
    fi
done

tmp=$(mktemp -d /tmp/clearmic-monitor-e2e.XXXXXX)
runtime="$tmp/runtime"
mkdir -p "$runtime" "$tmp/xdg"
chmod 700 "$runtime"
cp /usr/share/pipewire/minimal.conf "$tmp/pipewire.conf"
cat >> "$tmp/pipewire.conf" <<'CONF'
context.objects = [
  { factory = spa-node-factory args = { factory.name = support.node.driver node.name = ClearMic-Monitor-Dummy-Driver node.group = pipewire.dummy priority.driver = 20000 node.always-process = true } }
]
CONF

export XDG_RUNTIME_DIR="$runtime"
export PIPEWIRE_RUNTIME_DIR="$runtime"
export PIPEWIRE_REMOTE=pipewire-0
export XDG_CONFIG_HOME="$tmp/xdg"
pipewire_pid=""
wireplumber_pid=""
source_pid=""
monitor_pid=""
start_pipewire() {
    pipewire -c "$tmp/pipewire.conf" >>"$tmp/pipewire.log" 2>&1 &
    pipewire_pid=$!
}
start_source() {
    "$source_path" >>"$tmp/source.log" 2>&1 &
    source_pid=$!
}
cleanup() {
    for pid in "$source_pid" "$monitor_pid" "$wireplumber_pid" "$pipewire_pid"; do
        if [[ -n "$pid" ]]; then kill -INT "$pid" 2>/dev/null || true; fi
    done
    for pid in "$source_pid" "$monitor_pid" "$wireplumber_pid" "$pipewire_pid"; do
        if [[ -n "$pid" ]]; then wait "$pid" 2>/dev/null || true; fi
    done
    rm -rf "$tmp"
}
trap cleanup EXIT

start_pipewire
dbus-run-session -- wireplumber >"$tmp/wireplumber.log" 2>&1 &
wireplumber_pid=$!
wait_for_pipewire() {
    for _ in $(seq 1 100); do
        if pw-cli ls Node >/dev/null 2>&1; then return 0; fi
        sleep 0.1
    done
    cat "$tmp/pipewire.log" "$tmp/wireplumber.log" >&2
    echo "Isolated PipeWire daemon did not become available" >&2
    return 1
}
wait_for_node_state() {
    local should_exist=$1
    for _ in $(seq 1 100); do
        local exists=false
        if pw-cli ls Node 2>/dev/null | grep -q 'node.name = "clearmic_e2e_source"'; then exists=true; fi
        if [[ "$exists" == "$should_exist" ]]; then return 0; fi
        sleep 0.1
    done
    echo "PipeWire synthetic source state did not become $should_exist" >&2
    pw-cli ls Node >&2 || true
    return 1
}
change_count() { awk '/^CHANGE / { count = $2 } END { print count + 0 }' "$tmp/monitor.log"; }
wait_for_new_change() {
    local previous=$1
    for _ in $(seq 1 100); do
        local current
        current=$(change_count)
        if (( current > previous )); then return 0; fi
        sleep 0.1
    done
    cat "$tmp/monitor.log" >&2
    echo "PipeWire device monitor did not report a change after count $previous" >&2
    return 1
}

wait_for_pipewire
start_source
wait_for_node_state true
"$monitor_path" >"$tmp/monitor.log" 2>&1 &
monitor_pid=$!
wait_for_new_change 0
count=$(change_count)

kill -INT "$source_pid"
wait "$source_pid"
source_pid=""
wait_for_node_state false
wait_for_new_change "$count"
count=$(change_count)

start_source
wait_for_node_state true
wait_for_new_change "$count"
count=$(change_count)

kill -TERM "$pipewire_pid"
wait "$pipewire_pid" 2>/dev/null || true
pipewire_pid=""
for _ in $(seq 1 50); do
    if ! pw-cli ls Node >/dev/null 2>&1; then break; fi
    sleep 0.1
done
start_pipewire
wait_for_pipewire
start_source
wait_for_node_state true
wait_for_new_change "$count"

printf 'PipeWire device monitor changes observed: %s\n' "$(change_count)"
