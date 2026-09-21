#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 The Isotone authors
#
# The portal tests: mock_portal.py in a process of its own, then the cases in
# ui_model_tests against it. Run inside a session bus of its own
# (ctest: dbus-run-session -- bash run_portal_test.sh <ui_model_tests>).
#
# Two runs of the binary, because the Background case is the app inside a
# Flatpak and FLATPAK_ID changes more than launch at sign-in: it is set for that
# run alone. The mock writes the autostart entry where the portal would, and
# ISOTONE_AUTOSTART_DIR points the app at the same directory, so neither the
# test's own nor anyone's real ~/.config/autostart is touched.
set -euo pipefail
tests="$1"
log="$(mktemp)"
autostart="$(mktemp -d)"
export MOCK_LOG="$log" REFUSE=mute ACTIVATE=eq MOCK_AUTOSTART_DIR="$autostart" MOCK_APP_ID=io.github.jackwangxyw.Isotone
python3 "$(dirname "${BASH_SOURCE[0]}")/mock_portal.py" &
mock=$!
trap 'kill "$mock" 2>/dev/null || true; rm -rf "$log" "$log.refuse" "$autostart"' EXIT
for _ in $(seq 1 100); do grep -q ready "$log" && break; sleep 0.05; done
ISOTONE_MOCK_PORTAL_LOG="$log" QT_QPA_PLATFORM=offscreen "$tests" "-tc=global hotkeys bind through the GlobalShortcuts portal*"

ISOTONE_MOCK_PORTAL_LOG="$log" QT_QPA_PLATFORM=offscreen FLATPAK_ID=io.github.jackwangxyw.Isotone \
    ISOTONE_AUTOSTART_DIR="$autostart" "$tests" "-tc=launch at sign-in in a Flatpak*"

# And a third, against a second mock that refuses the shortcuts session
# outright. It is a second portal rather than another request to the first,
# because the first one would have to stop answering to play both parts.
kill "$mock" 2>/dev/null || true
refuse_log="$(mktemp)"
MOCK_LOG="$refuse_log" REFUSE_SESSION=1 python3 "$(dirname "${BASH_SOURCE[0]}")/mock_portal.py" &
mock=$!
trap 'kill "$mock" 2>/dev/null || true; rm -rf "$log" "$log.refuse" "$autostart" "$refuse_log"' EXIT
for _ in $(seq 1 100); do grep -q ready "$refuse_log" && break; sleep 0.05; done
ISOTONE_MOCK_PORTAL_LOG="$refuse_log" QT_QPA_PLATFORM=offscreen "$tests" "-tc=a desktop that refuses the shortcuts session*"
