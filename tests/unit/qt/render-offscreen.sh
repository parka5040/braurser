#!/usr/bin/env bash
set -euo pipefail
umask 077
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../.." && pwd)
output=${1:?output directory required}
mkdir -p -- "$output"
output=$(cd -- "$output" && pwd)
runtime_parent=$output
if [[ ${TMPDIR:-} == /* ]]; then
    runtime_parent=$TMPDIR
fi
runtime=$(mktemp -d "$runtime_parent/braurser-offscreen.XXXXXX")
cleanup() {
    rmdir -- "$runtime" 2>/dev/null || true
}
trap cleanup EXIT INT TERM
failures=()
for width in 360 600 1024 1920; do
    for theme in light dark; do
        for page in discover search details updates installed settings activity git; do
            name="qt-app-$page-$theme-$width.png"
            if env -u WAYLAND_DISPLAY -u DISPLAY XDG_RUNTIME_DIR="$runtime" DBUS_SESSION_BUS_ADDRESS="unix:path=$runtime/no-session-bus" QT_QPA_PLATFORM=offscreen QT_SCALE_FACTOR=1 QT_FONT_DPI=96 timeout 20 "$root/build/release/bin/braurser-qt" --render-png "$output/$name" --page "$page" --theme "$theme" --width "$width"; then
                printf 'Rendered %s\n' "$name"
            else
                code=$?
                failures+=("$name (exit $code)")
                printf 'FAILED: %s (exit %s)\n' "$name" "$code" >&2
            fi
        done
    done
done
if ((${#failures[@]})); then
    printf '\n%s Qt render failures:\n' "${#failures[@]}" >&2
    printf '  %s\n' "${failures[@]}" >&2
    exit 1
fi
printf '%s\n' 'All 64 Qt app renders completed successfully.'
