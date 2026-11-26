#!/bin/sh
set -eu
mode=live
case ${1-} in live|panel|trace) mode=$1; shift ;; esac
[ "$#" -gt 0 ] || { echo 'Usage: elite-layer.sh [live|panel|trace] COMMAND [ARG ...]' >&2; exit 2; }
root=$(CDPATH= cd -P "$(dirname "$0")" && pwd)
IFS= read -r data < "$root/data-path"
XR_API_LAYER_PATH=$root/layer${XR_API_LAYER_PATH:+:$XR_API_LAYER_PATH}
XR_ENABLE_API_LAYERS=XR_APILAYER_EDMO_overlay${XR_ENABLE_API_LAYERS:+:$XR_ENABLE_API_LAYERS}
export XR_API_LAYER_PATH XR_ENABLE_API_LAYERS
export EDMO_XR_LOG="${EDMO_XR_LOG:--}"
export EDMO_XR_MODE="$mode" PRESSURE_VESSEL_IMPORT_OPENXR_1_LAYERS=1
if [ "$mode" = live ]; then
    export EDMO_DISPLAY_FILE="$data/display.json"
    export EDMO_XR_CHANNEL="$data/frame-1280x960.shm"
fi
exec "$@"
