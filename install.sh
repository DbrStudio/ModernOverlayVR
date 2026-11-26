#!/bin/sh
set -eu
usage() {
    echo 'Usage: install.sh [--prefix ABSOLUTE_DIR] [--edmc native|flatpak | --plugins ABSOLUTE_DIR]'
}
prefix=${XDG_DATA_HOME:-"$HOME/.local/share"}/ModernOverlayVR
native=${XDG_DATA_HOME:-"$HOME/.local/share"}/EDMarketConnector/plugins
flatpak=$HOME/.var/app/io.edcd.EDMarketConnector/data/EDMarketConnector/plugins
plugins=
while [ "$#" -gt 0 ]; do
    case $1 in
        --help|-h) usage; exit 0 ;;
        --prefix|--edmc|--plugins) [ "$#" -ge 2 ] || { usage >&2; exit 2; } ;;
        *) usage >&2; exit 2 ;;
    esac
    case $1 in
        --prefix) prefix=$2 ;;
        --plugins) plugins=$2 ;;
        --edmc) case $2 in
            native) plugins=$native ;;
            flatpak) plugins=$flatpak ;;
            *) usage >&2; exit 2 ;;
        esac ;;
    esac
    shift 2
done
if [ -z "$plugins" ]; then
    if [ -d "$flatpak" ] && [ -d "$native" ]; then
        echo 'Both plugin directories exist. Select --edmc native or --edmc flatpak.' >&2
        exit 2
    fi
    plugins=$native
    if [ -d "$flatpak" ]; then plugins=$flatpak; fi
fi
for dir in "$prefix" "$plugins"; do
    case $dir in /*) ;; *) echo "Expected absolute directory: $dir" >&2; exit 2 ;; esac
done
root=$(CDPATH= cd -P "$(dirname "$0")" && pwd)
build=$(mktemp -d)
trap 'rm -rf "$build"' 0
cmake -S "$root" -B "$build" -DBUILD_TESTING=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build "$build" --parallel
mkdir -p "$prefix/layer" "$plugins/ModernOverlayVR/renderer"
plugins=$(CDPATH= cd -P "$plugins" && pwd)
# Rename complete files so readers never see a partially copied library or script.
publish() (
    tmp=$(mktemp "$2.tmp.XXXXXX")
    trap 'rm -f "$tmp"' 0
    cp "$1" "$tmp"
    chmod "$3" "$tmp"
    mv -f "$tmp" "$2"
)
for file in edmo.json libedmo_openxr.so; do
    publish "$build/layer/$file" "$prefix/layer/$file" 644
done
dest=$plugins/ModernOverlayVR
for file in modern.py channel.py; do publish "$root/renderer/$file" "$dest/renderer/$file" 644; done
for file in vr_settings.py load.py; do publish "$root/edmc/$file" "$dest/$file" 644; done
printf '%s/edmo-vr\n' "$(dirname "$plugins")" > "$build/data-path"
publish "$build/data-path" "$prefix/data-path" 644
printf '%s\n' "$plugins" > "$build/plugins-path"
publish "$build/plugins-path" "$prefix/plugins-path" 644
publish "$root/scripts/overlay-test" "$prefix/overlay-test" 755
publish "$root/scripts/elite-layer.sh" "$prefix/elite-layer.sh" 755
printf 'Installed wrapper: %s/elite-layer.sh\nInstalled plugin: %s\n' "$prefix" "$dest"
