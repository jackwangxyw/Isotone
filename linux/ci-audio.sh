#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 The Isotone authors
#
# Runs the Linux measurements against a PipeWire started for this run alone: its
# own runtime and config directories, its own D-Bus session, and no systemd user
# manager. Both ends of the chain are null sinks, so no audio hardware is
# involved, which is what makes the stage 1c and stage 3 measurements runnable
# on a CI runner.
#
# WirePlumber has to run. Not for policy, since every link the rig needs is made
# explicitly, but because an adapter node has no DSP ports until a session
# manager sets PortConfig mode=dsp on it. Started with no session manager, the
# declared sinks appear in the graph with no ports at all, which looks exactly
# like PipeWire having ignored the configuration.

set -euo pipefail

# WirePlumber needs a session bus. Re-exec under one rather than require the
# caller to have provided it.
if [ -z "${DBUS_SESSION_BUS_ADDRESS:-}" ]; then
    # Through bash, not "$0" on its own: this is run as `bash linux/ci-audio.sh`
    # and carries no executable bit, so dbus-run-session cannot exec it directly
    # and fails with Permission denied.
    exec dbus-run-session -- bash "$0" "$@"
fi

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
scratch="${RUNNER_TEMP:-/tmp}/isotone-ci"

export XDG_RUNTIME_DIR="$scratch/runtime"
export XDG_CONFIG_HOME="$scratch/config"
export ISOTONE_MEASURE_NO_RESTART=1
# CI builds into build/ at the root; an interactive run sets this to its own
# build directory before calling the script.
export ISOTONE_BUILD_DIR="${ISOTONE_BUILD_DIR:-$root/build}"
export ISOTONE_MEASURE_WORK="$scratch/work"

rm -rf "$scratch"
mkdir -p "$XDG_RUNTIME_DIR" "$XDG_CONFIG_HOME/pipewire/pipewire.conf.d"
chmod 700 "$XDG_RUNTIME_DIR"
cp "$root/linux/spike/10-isotone-spike.conf" "$XDG_CONFIG_HOME/pipewire/pipewire.conf.d/"

pipewire &
pipewire_pid=$!
wireplumber &
wireplumber_pid=$!
cleanup() { kill "$wireplumber_pid" "$pipewire_pid" 2>/dev/null || true; }
trap cleanup EXIT

echo "waiting for the declared sinks"
for _ in $(seq 1 150); do
    if pw-link -i 2>/dev/null | grep -q '^isotone_hw:'; then
        break
    fi
    sleep 0.1
done
if ! pw-link -i 2>/dev/null | grep -q '^isotone_hw:'; then
    echo "PipeWire came up without the rig's sinks" >&2
    pw-cli ls Node 2>&1 | head -40 >&2
    exit 1
fi
# Informational only, and under `set -euo pipefail` a grep that matches
# nothing, or one killed by SIGPIPE from head, would end the run here with
# the sinks already up and no measurement attempted.
pw-cli info 0 2>/dev/null | grep -E 'version|name' | head -3 || true

echo
echo "=== stage 1c: the topology spike ==="
python3 "$root/linux/spike/measure.py"
echo
echo "=== stage 3: the daemon ==="
python3 "$root/linux/daemon/measure.py"
