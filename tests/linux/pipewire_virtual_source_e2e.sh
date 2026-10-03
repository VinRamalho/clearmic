#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cli_path=${1:-"$repo_root/build/clearmic-cli"}
if [[ "$cli_path" != /* ]]; then cli_path="$repo_root/$cli_path"; fi
if [[ ! -x "$cli_path" ]]; then
    echo "ClearMic CLI executable not found: $cli_path" >&2
    exit 2
fi
for command in pipewire wireplumber pw-cli pw-cat timeout python3; do
    if ! command -v "$command" >/dev/null; then
        echo "PipeWire integration test requires '$command'" >&2
        exit 2
    fi
done

tmp=$(mktemp -d /tmp/clearmic-route-e2e.XXXXXX)
runtime="$tmp/runtime"
mkdir -p "$runtime" "$tmp/xdg"
chmod 700 "$runtime"
cp /usr/share/pipewire/minimal.conf "$tmp/pipewire.conf"
python3 - "$tmp/pipewire.conf" <<'PY'
import sys

config_path = sys.argv[1]
with open(config_path, encoding="utf-8") as config_file:
    config = config_file.read()
needle = "    audio.convert.* = audioconvert/libspa-audioconvert"
if needle not in config:
    raise SystemExit("PipeWire minimal.conf does not contain the expected SPA library section")
config = config.replace(
    needle,
    "    audiotestsrc    = audiotestsrc/libspa-audiotestsrc\n" + needle,
    1,
)
config += '''
context.objects = [
  { factory = adapter args = { factory.name = audiotestsrc node.name = clearmic_e2e_source node.description = "ClearMic E2E Test Source" media.class = Audio/Source audio.channels = 1 audio.position = [ MONO ] node.param.Props = { live = true } } }
  { factory = spa-node-factory args = { factory.name = support.node.driver node.name = ClearMic-E2E-Dummy-Driver node.group = pipewire.dummy priority.driver = 20000 node.always-process = true } }
]
'''
with open(config_path, "w", encoding="utf-8") as config_file:
    config_file.write(config)
PY

export XDG_RUNTIME_DIR="$runtime"
export PIPEWIRE_RUNTIME_DIR="$runtime"
export PIPEWIRE_REMOTE=pipewire-0
export XDG_CONFIG_HOME="$tmp/xdg"
pipewire -c "$tmp/pipewire.conf" >"$tmp/pipewire.log" 2>&1 &
pipewire_pid=$!
wireplumber >"$tmp/wireplumber.log" 2>&1 &
wireplumber_pid=$!
service_pid=""
consumer_pid=""
cleanup() {
    if [[ -n "$consumer_pid" ]]; then
        kill -INT "$consumer_pid" 2>/dev/null || true
        wait "$consumer_pid" 2>/dev/null || true
    fi
    if [[ -n "$service_pid" ]]; then
        kill -INT "$service_pid" 2>/dev/null || true
        wait "$service_pid" 2>/dev/null || true
    fi
    kill "$wireplumber_pid" "$pipewire_pid" 2>/dev/null || true
    wait "$wireplumber_pid" "$pipewire_pid" 2>/dev/null || true
    rm -rf "$tmp"
}
trap cleanup EXIT

ready=false
for _ in $(seq 1 50); do
    if pw-cli ls Node 2>/dev/null | grep -q 'node.name = "clearmic_e2e_source"'; then
        ready=true
        break
    fi
    sleep 0.1
done
if [[ "$ready" != true ]]; then
    cat "$tmp/pipewire.log" "$tmp/wireplumber.log" >&2
    echo "Isolated PipeWire source did not become available" >&2
    exit 1
fi

device_list=$("$cli_path" devices)
printf '%s\n' "$device_list"
if ! grep -q 'ClearMic E2E Test Source' <<<"$device_list"; then
    echo "ClearMic did not enumerate the synthetic PipeWire microphone" >&2
    exit 1
fi

timeout --signal=INT --kill-after=2 6 "$cli_path" serve "" natural --noise-suppression=off >"$tmp/clearmic.log" 2>&1 &
service_pid=$!
virtual_source_ready=false
source_id=""
source_info=""
for _ in $(seq 1 50); do
    node_list=$(pw-cli ls Node 2>/dev/null || true)
    source_id=$(awk '/^[[:space:]]*id / { id = $2; sub(/,/, "", id) } /node.name = "clearmic_virtual_microphone"/ { print id; exit }' <<<"$node_list")
    if [[ -n "$source_id" ]]; then
        source_info=$(pw-cli info "$source_id" 2>/dev/null || true)
        if grep -q 'node.description = "ClearMic Virtual Microphone"' <<<"$source_info" &&
           grep -q 'media.class = "Audio/Source"' <<<"$source_info"; then
            virtual_source_ready=true
            break
        fi
    fi
    sleep 0.1
done
if [[ "$virtual_source_ready" != true ]]; then
    printf '%s\n' "$node_list" >&2
    printf 'Source id: %s\n%s\n' "$source_id" "$source_info" >&2
    cat "$tmp/clearmic.log" >&2
    echo "ClearMic did not publish an Audio/Source node" >&2
    exit 1
fi

timeout --signal=INT --kill-after=2 3 pw-cat --record --target clearmic_virtual_microphone \
    --rate 48000 --channels 1 --format s16 "$tmp/output.wav" >"$tmp/consumer.log" 2>&1 &
consumer_pid=$!
linked=false
for _ in $(seq 1 20); do
    links=$(pw-cli ls Link 2>/dev/null || true)
    if grep -q "link.output.node = \"$source_id\"" <<<"$links"; then
        linked=true
        break
    fi
    sleep 0.1
done
if [[ "$linked" != true ]]; then
    cat "$tmp/consumer.log" >&2
    echo "The PipeWire consumer did not link to ClearMic's virtual source" >&2
    exit 1
fi
wait "$consumer_pid" || true
consumer_pid=""
kill -INT "$service_pid" 2>/dev/null || true
wait "$service_pid" 2>/dev/null || true
service_pid=""
cat "$tmp/clearmic.log"

python3 - "$tmp/output.wav" <<'PY'
import math
import struct
import sys
import wave

with wave.open(sys.argv[1], "rb") as wav_file:
    if wav_file.getframerate() != 48000 or wav_file.getnchannels() != 1:
        raise SystemExit("ClearMic virtual microphone returned an unexpected format")
    frames = wav_file.readframes(wav_file.getnframes())
samples = struct.unpack("<" + "h" * (len(frames) // 2), frames[: len(frames) // 2 * 2])
rms = math.sqrt(sum(sample * sample for sample in samples) / len(samples)) if samples else 0.0
print(f"Virtual microphone capture: {len(samples)} frames, RMS {rms:.1f}")
if len(samples) < 48000 or rms < 1000:
    raise SystemExit("ClearMic virtual microphone did not deliver non-silent processed audio")
PY
