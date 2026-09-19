#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 The Isotone authors
#
# The GlobalShortcuts portal test: mock_portal.py in a process of its own, then
# the test case in ui_model_tests against it. Run inside a session bus of its own
# (ctest: dbus-run-session -- bash run_portal_test.sh <ui_model_tests>).
set -euo pipefail
tests="$1"
log="$(mktemp)"
export MOCK_LOG="$log" REFUSE=mute ACTIVATE=eq
python3 "$(dirname "${BASH_SOURCE[0]}")/mock_portal.py" &
mock=$!
trap 'kill "$mock" 2>/dev/null || true; rm -f "$log"' EXIT
for _ in $(seq 1 100); do grep -q ready "$log" && break; sleep 0.05; done
ISOTONE_MOCK_PORTAL_LOG="$log" QT_QPA_PLATFORM=offscreen "$tests" "-tc=global hotkeys bind through the GlobalShortcuts portal*"
