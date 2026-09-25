#!/usr/bin/env bash
set -euo pipefail
umask 077
output=${1:?output directory required}
width=${2:-360}
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../.." && pwd)
gallery=${BRAURSER_GALLERY:-"$root/build/release/bin/braurser-gtk-gallery"}
[[ $width =~ ^[0-9]+$ ]] && ((width >= 280 && width <= 3840))
[[ -n ${WAYLAND_DISPLAY:-} ]]
command -v grim >/dev/null
[[ -x $gallery ]]
mkdir -p -- "$output"
chmod 700 -- "$output"
pid=
cleanup() {
    if [[ -n $pid ]] && kill -0 "$pid" 2>/dev/null; then
        kill "$pid"
        wait "$pid" || true
    fi
}
trap cleanup EXIT INT TERM
for theme in light dark; do
    for page in cards badges controls banners progress document empty; do
        log="$output/$page-$theme-$width.log"
        env -u GTK_THEME GDK_BACKEND=wayland "$gallery" --page "$page" --theme "$theme" --width "$width" --exit-after-ms 10000 >"$log" 2>&1 &
        pid=$!
        ready=0
        for ((attempt=0; attempt<80; ++attempt)); do
            while IFS= read -r line; do
                if [[ $line == allocation\ * ]]; then ready=1; fi
            done < "$log"
            if ((ready)); then break; fi
            kill -0 "$pid" 2>/dev/null || { printf 'Gallery exited before mapping: %s\n' "$log" >&2; exit 1; }
            sleep 0.1
        done
        ((ready)) || { printf 'Gallery did not map: %s\n' "$log" >&2; exit 1; }
        sleep 0.5
        grim "$output/$page-$theme-$width.png"
        kill "$pid"
        wait "$pid" || true
        pid=
    done
done
