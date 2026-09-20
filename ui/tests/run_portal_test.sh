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
export MOCK_LOG="$log" REFUSE=mute ACTIVATE=eq MOCK_AUTOSTART_DIR="$autostart" MOCK_APP_ID=io.github.isotone.Isotone
python3 "$(dirname "${BASH_SOURCE[0]}")/mock_portal.py" &
mock=$!
trap 'kill "$mock" 2>/dev/null || true; rm -rf "$log" "$log.refuse" "$autostart"' EXIT
for _ in $(seq 1 100); do grep -q ready "$log" && break; sleep 0.05; done
ISOTONE_MOCK_PORTAL_LOG="$log" QT_QPA_PLATFORM=offscreen "$tests" "-tc=global hotkeys bind through the GlobalShortcuts portal*"

ISOTONE_MOCK_PORTAL_LOG="$log" QT_QPA_PLATFORM=offscreen FLATPAK_ID=io.github.isotone.Isotone \
    ISOTONE_AUTOSTART_DIR="$autostart" "$tests" "-tc=launch at sign-in in a Flatpak*"
