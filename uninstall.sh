#!/bin/sh
set -eu

usage() {
    echo 'Usage: uninstall.sh [--prefix ABSOLUTE_DIR]'
}

prefix=${XDG_DATA_HOME:-"$HOME/.local/share"}/ModernOverlayVR
case ${1-} in
    '') [ "$#" -eq 0 ] || { usage >&2; exit 2; } ;;
    --help|-h) usage; exit 0 ;;
    --prefix) [ "$#" -eq 2 ] || { usage >&2; exit 2; }; prefix=$2 ;;
    *) usage >&2; exit 2 ;;
esac
case $prefix in
    /*) ;;
    *) usage >&2; exit 2 ;;
esac

if [ ! -f "$prefix/plugins-path" ]; then
    echo "No ModernOverlayVR installation record at $prefix/plugins-path."
    exit 0
fi
IFS= read -r plugins < "$prefix/plugins-path"
case $plugins in
    /*) ;;
    *) echo "Invalid plugin path in $prefix/plugins-path" >&2; exit 1 ;;
esac

dest=$plugins/ModernOverlayVR
rm -f "$dest/load.py" "$dest/vr_settings.py" "$dest/renderer/modern.py" "$dest/renderer/channel.py"
rm -f "$prefix/layer/edmo.json" "$prefix/layer/libedmo_openxr.so" \
    "$prefix/elite-layer.sh" "$prefix/overlay-test" "$prefix/data-path" "$prefix/plugins-path"
# Leave unrelated files and directories containing them in place.
rmdir "$dest/renderer" "$dest" "$prefix/layer" "$prefix" 2>/dev/null || :
echo 'ModernOverlayVR removed. Settings and runtime files in edmo-vr were preserved.'
