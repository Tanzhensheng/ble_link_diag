#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROBOT_REPO="${ROBOT_REPO:-$HOME/bluetooth/csp_whl_robot}"
OUT="${OUT:-$SCRIPT_DIR/ble_link_diag}"
CC="${CC:-gcc}"

if [ ! -f "$ROBOT_REPO/src/platform/module/mod_ble/ble_link.c" ]; then
    echo "Cannot find ble_link.c under ROBOT_REPO=$ROBOT_REPO" >&2
    echo "Set ROBOT_REPO=/path/to/csp_whl_robot and retry." >&2
    exit 1
fi

pkg-config --exists gio-2.0 glib-2.0

"$CC" -Wall -Wextra -O0 -g \
    -I"$SCRIPT_DIR/stubs" \
    -I"$ROBOT_REPO/src/platform/module/mod_ble" \
    "$SCRIPT_DIR/ble_link_diag.c" \
    "$ROBOT_REPO/src/platform/module/mod_ble/ble_link.c" \
    -o "$OUT" \
    $(pkg-config --cflags --libs gio-2.0 glib-2.0)

echo "Built: $OUT"
